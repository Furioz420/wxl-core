// Opt-in native allocation ownership diagnostic. Does not change allocation policy.
#include "AllocationLedger.hpp"
#include "MemoryDiagnosticConfig.hpp"
#include "common/Config.hpp"
#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "offsets/engine/Mem.hpp"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cstring>

namespace {
using namespace wxl::diag;
constexpr uint32_t kMinimum=16*1024;
constexpr uintptr_t kReallocate=0x0076E5E0; // build 12340, five stdcall arguments, ret 14h
using ReallocateFn=void*(__stdcall*)(void*,uint32_t,const char*,int,uint32_t);
wxl::offsets::engine::mem::Mem_AllocFn g_alloc=nullptr;
using FreeFn=int(__stdcall*)(void*,const char*,int,uint32_t); // native returns 1
FreeFn g_free=nullptr;
ReallocateFn g_realloc=nullptr;
AllocationLedger<65536> g_records;
SRWLOCK g_lock=SRWLOCK_INIT;
volatile LONG g_reportAt=0;
thread_local unsigned g_depth=0;
struct Depth {Depth(){++g_depth;}~Depth(){--g_depth;}};
struct Group {AllocationSite site{};uint64_t bytes=0;uint32_t count=0;};

void TagText(uintptr_t address,char* out) noexcept {
    out[0]=0;
    __try {if(address) {size_t i=0;for(;i<79;++i){char c=reinterpret_cast<const char*>(address)[i];if(!c)break;out[i]=(c>=32 && c<127)?c:'?';}out[i]=0;}}
    __except(EXCEPTION_EXECUTE_HANDLER){strcpy(out,"<unreadable>");}
}
void Report(bool force=false) {
    const DWORD now=GetTickCount();const LONG last=InterlockedCompareExchange(&g_reportAt,0,0);
    if(!force && now-static_cast<DWORD>(last)<1000)return;
    if(!force && InterlockedCompareExchange(&g_reportAt,static_cast<LONG>(now),last)!=last)return;
    if(force)InterlockedExchange(&g_reportAt,static_cast<LONG>(now));
    Group groups[1024]{};size_t used=0;uint64_t ungrouped=0,total=0,missed=0;size_t count=0;
    AcquireSRWLockShared(&g_lock);
    total=g_records.bytes;count=g_records.count;missed=g_records.missed;
    g_records.Each([&](const AllocationRecord& r){
        size_t i=0;for(;i<used;++i)if(groups[i].site==r.site)break;
        if(i==used){if(used==1024){ungrouped+=r.bytes;return;}groups[used++].site=r.site;}
        groups[i].bytes+=r.bytes;++groups[i].count;
    });
    ReleaseSRWLockShared(&g_lock);
    std::sort(groups,groups+used,[](const Group& a,const Group& b){return a.bytes>b.bytes;});
    WLOG_INFO("native-heap-v1: live=%u requested_mb=%.1f minimum_bytes=%u missed=%llu ungrouped_mb=%.1f",
        unsigned(count),total/1048576.0,kMinimum,static_cast<unsigned long long>(missed),ungrouped/1048576.0);
    for(size_t i=0;i<(std::min)(used,size_t(12));++i){char tag[80];TagText(groups[i].site.tag,tag);
        WLOG_INFO("native-heap-site: rank=%u caller=%08X parent=%08X tag=%08X line=%u name=%s live=%u requested_mb=%.1f",
            unsigned(i+1),unsigned(groups[i].site.caller),unsigned(groups[i].site.parent),unsigned(groups[i].site.tag),groups[i].site.line,tag,groups[i].count,groups[i].bytes/1048576.0);
    }
}
__declspec(noinline) AllocationSite Site(uintptr_t caller,const char* tag,int line){
    void* frames[3]{};
    // Best-effort caller context for generic TSArray allocation helpers. A short
    // stack remains valid telemetry with parent=0; no symbols are loaded here.
    const USHORT count=CaptureStackBackTrace(2,3,frames,nullptr);
    return {caller,reinterpret_cast<uintptr_t>(tag),uint32_t(line),count>1?reinterpret_cast<uintptr_t>(frames[1]):0};
}
AllocationRecord Take(void* p){AcquireSRWLockExclusive(&g_lock);auto r=g_records.Remove(reinterpret_cast<uintptr_t>(p));ReleaseSRWLockExclusive(&g_lock);return r;}
void Put(void* p,uint32_t n,AllocationSite site){if(!p||n<kMinimum)return;AcquireSRWLockExclusive(&g_lock);g_records.Add({reinterpret_cast<uintptr_t>(p),n,site});ReleaseSRWLockExclusive(&g_lock);}
void* __stdcall Allocate(uint32_t n,const char* tag,int line,uint32_t flags){
    if(g_depth)return g_alloc(n,tag,line,flags);
    Depth depth;
    if(n>=kMinimum)Report(); // Reports during synchronous loading, before a fatal native allocation.
    void* p=g_alloc(n,tag,line,flags);
    if(p && n>=kMinimum)Put(p,n,Site(reinterpret_cast<uintptr_t>(_ReturnAddress()),tag,line));
    return p;
}
int __stdcall Free(void* p,const char* tag,int line,uint32_t flags){
    if(g_depth)return g_free(p,tag,line,flags);
    Depth depth;Take(p);return g_free(p,tag,line,flags);
}
void* __stdcall Reallocate(void* p,uint32_t n,const char* tag,int line,uint32_t flags){
    if(g_depth)return g_realloc(p,n,tag,line,flags);
    Depth depth;if(n>=kMinimum)Report();
    const auto old=Take(p);
    void* result=g_realloc(p,n,tag,line,flags);
    if(result){if(n>=kMinimum)Put(result,n,Site(reinterpret_cast<uintptr_t>(_ReturnAddress()),tag,line));}
    // Verified special no-op flag paths return null without freeing; ordinary
    // realloc failure preserves the old block except size-zero free.
    else if(old.pointer && (n || flags==0xB00BEEE5u || (p && (flags&0x10))))Put(p,old.bytes,old.site);
    return result;
}
void Tick(void*,const void*){if(!g_depth){Depth depth;Report();}}
bool Install(){
    if(!wxl::diag::MemoryOwnersEnabled())return true;
    // Version-sensitive signatures: fail closed if this is not the inspected client.
    const uint8_t alloc[]={0x55,0x8B,0xEC,0x56,0x57};
    const uint8_t free[]={0x55,0x8B,0xEC,0x56,0x8B,0x75,0x08};
    const uint8_t realloc[]={0x55,0x8B,0xEC,0x51,0x53};
    namespace m=wxl::offsets::engine::mem;
    if(memcmp(reinterpret_cast<void*>(m::kAlloc),alloc,sizeof alloc)||memcmp(reinterpret_cast<void*>(m::kFree),free,sizeof free)||memcmp(reinterpret_cast<void*>(kReallocate),realloc,sizeof realloc)){
        WLOG_WARN("native-heap-v1: signature mismatch, diagnostic disabled");return true;}
    wxl::hook::Install("Diagnostic.NativeAlloc",m::kAlloc,&Allocate,&g_alloc);
    wxl::hook::Install("Diagnostic.NativeFree",m::kFree,&Free,&g_free);
    wxl::hook::Install("Diagnostic.NativeRealloc",kReallocate,&Reallocate,&g_realloc);
    wxl::events::Subscribe(wxl::events::Event::OnUpdate,&Tick,nullptr);
    WLOG_INFO("native-heap-v1: enabled, tracks >=16 KiB native blocks; fixed ledger=%u bytes",unsigned(sizeof g_records));
    return true;
}
}
WXL_REGISTER_FEATURE("native-allocation-diagnostic",true,Install)
