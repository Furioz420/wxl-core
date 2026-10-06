// Diagnostic resource lifetimes; never takes a reference to the observed resource.
#include "MemoryDiagnosticConfig.hpp"
#include "common/Log.hpp"
#include "common/Mem.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "game/Gx.hpp"
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <new>
#include <algorithm>

namespace {
constexpr GUID kLifetimeKey={0xfec64b02,0x4d71,0x431a,{0xb9,0xe5,0xda,0x19,0xc2,0x86,0xe7,0x31}};
enum Kind {Texture,Volume,Cube,Vertex,Index,RenderTarget,DepthSurface,Offscreen,KindCount};
constexpr const char* kNames[]={"texture","volume","cube","vertex","index","render-target","depth","offscreen"};
struct Counter {std::atomic<uint64_t> bytes{0},created{0},destroyed{0},unknown{0};std::atomic<uint32_t> live{0};};
Counter g_counters[KindCount][4];
std::atomic<uint64_t> g_untracked{0},g_createFailed{0};
volatile LONG g_reportAt=0;

// Destruction follows D3D private-data lifetime, not an estimated reference count.
// This object holds only counters, never a resource/device reference, so no cycle.
class Lifetime final : public IUnknown {
    volatile LONG refs_=1;
    Counter* counter_;uint64_t bytes_;bool unknown_;
public:
    Lifetime(Counter* c,uint64_t bytes,bool unknown):counter_(c),bytes_(bytes),unknown_(unknown){
        ++c->live;++c->created;c->bytes.fetch_add(bytes);if(unknown)++c->unknown;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(iid==__uuidof(IUnknown)){*out=static_cast<IUnknown*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release() override{
        const LONG left=InterlockedDecrement(&refs_);if(!left)delete this;return left;
    }
    ~Lifetime(){--counter_->live;++counter_->destroyed;counter_->bytes.fetch_sub(bytes_);if(unknown_)--counter_->unknown;}
};
uint64_t LevelBytes(UINT w,UINT h,UINT d,D3DFORMAT f){
    if(f==D3DFMT_DXT1)return uint64_t((w+3)/4)*((h+3)/4)*d*8;
    if(f==D3DFMT_DXT2||f==D3DFMT_DXT3||f==D3DFMT_DXT4||f==D3DFMT_DXT5)return uint64_t((w+3)/4)*((h+3)/4)*d*16;
    unsigned pixel=0;
    switch(f){
    case D3DFMT_A8:case D3DFMT_L8:case D3DFMT_P8:pixel=1;break;
    case D3DFMT_A8L8:case D3DFMT_A8P8:case D3DFMT_R5G6B5:case D3DFMT_X1R5G5B5:case D3DFMT_A1R5G5B5:case D3DFMT_A4R4G4B4:case D3DFMT_D16:case D3DFMT_D16_LOCKABLE:case D3DFMT_R16F:case D3DFMT_V8U8:pixel=2;break;
    case D3DFMT_R8G8B8:pixel=3;break;
    case D3DFMT_A8R8G8B8:case D3DFMT_X8R8G8B8:case D3DFMT_A8B8G8R8:case D3DFMT_X8B8G8R8:case D3DFMT_A2R10G10B10:case D3DFMT_G16R16:case D3DFMT_D24S8:case D3DFMT_D24X8:case D3DFMT_D32:case D3DFMT_R32F:case D3DFMT_G16R16F:pixel=4;break;
    case D3DFMT_A16B16G16R16:case D3DFMT_A16B16G16R16F:case D3DFMT_G32R32F:pixel=8;break;
    case D3DFMT_A32B32G32R32F:pixel=16;break;
    default:return 0;
    }return uint64_t(w)*h*d*pixel;
}
void Report(){
    const DWORD now=GetTickCount();const LONG last=InterlockedCompareExchange(&g_reportAt,0,0);
    if(now-DWORD(last)<1000 || InterlockedCompareExchange(&g_reportAt,LONG(now),last)!=last)return;
    WLOG_INFO("gpu-memory-v1: untracked=%llu create_failed=%llu (logical payload estimates, not process bytes)",
        static_cast<unsigned long long>(g_untracked.load()),static_cast<unsigned long long>(g_createFailed.load()));
    for(unsigned k=0;k<KindCount;++k)for(unsigned p=0;p<4;++p){auto& c=g_counters[k][p];if(!c.created.load())continue;
        WLOG_INFO("gpu-memory-kind: kind=%s pool=%u live=%u payload_mb=%.1f created=%llu destroyed=%llu unknown_live=%llu",
            kNames[k],p,c.live.load(),c.bytes.load()/1048576.0,static_cast<unsigned long long>(c.created.load()),
            static_cast<unsigned long long>(c.destroyed.load()),static_cast<unsigned long long>(c.unknown.load()));}
}
void Attach(IDirect3DResource9* r,Kind kind,D3DPOOL pool,uint64_t bytes){
    if(!r)return;
    if(unsigned(pool)>3){++g_untracked;return;}
    auto* lifetime=new(std::nothrow) Lifetime(&g_counters[kind][pool],bytes,bytes==0);
    if(!lifetime){++g_untracked;return;}
    const HRESULT hr=r->SetPrivateData(kLifetimeKey,static_cast<IUnknown*>(lifetime),sizeof(IUnknown*),D3DSPD_IUNKNOWN);
    if(FAILED(hr))++g_untracked;
    lifetime->Release(); // only the resource's private data retains the token
    Report();
}
using Device=IDirect3DDevice9;
using TextureFn=HRESULT(STDMETHODCALLTYPE*)(Device*,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DTexture9**,HANDLE*);
using VolumeFn=HRESULT(STDMETHODCALLTYPE*)(Device*,UINT,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DVolumeTexture9**,HANDLE*);
using CubeFn=HRESULT(STDMETHODCALLTYPE*)(Device*,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DCubeTexture9**,HANDLE*);
using VertexFn=HRESULT(STDMETHODCALLTYPE*)(Device*,UINT,DWORD,DWORD,D3DPOOL,IDirect3DVertexBuffer9**,HANDLE*);
using IndexFn=HRESULT(STDMETHODCALLTYPE*)(Device*,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DIndexBuffer9**,HANDLE*);
using SurfaceFn=HRESULT(STDMETHODCALLTYPE*)(Device*,UINT,UINT,D3DFORMAT,D3DMULTISAMPLE_TYPE,DWORD,BOOL,IDirect3DSurface9**,HANDLE*);
using OffscreenFn=HRESULT(STDMETHODCALLTYPE*)(Device*,UINT,UINT,D3DFORMAT,D3DPOOL,IDirect3DSurface9**,HANDLE*);
TextureFn g_texture=nullptr;VolumeFn g_volume=nullptr;CubeFn g_cube=nullptr;VertexFn g_vertex=nullptr;IndexFn g_index=nullptr;
SurfaceFn g_target=nullptr,g_depth=nullptr;OffscreenFn g_offscreen=nullptr;
HRESULT STDMETHODCALLTYPE TextureHook(Device* d,UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT f,D3DPOOL p,IDirect3DTexture9** out,HANDLE* shared){
    auto hr=g_texture(d,w,h,levels,usage,f,p,out,shared);if(SUCCEEDED(hr)&&out&&*out){uint64_t bytes=0;
        for(UINT i=0;i<(*out)->GetLevelCount();++i){D3DSURFACE_DESC desc{};if(SUCCEEDED((*out)->GetLevelDesc(i,&desc)))bytes+=LevelBytes(desc.Width,desc.Height,1,desc.Format);}
        Attach(*out,Texture,p,bytes);
    }else if(FAILED(hr))++g_createFailed;return hr;
}
HRESULT STDMETHODCALLTYPE VolumeHook(Device* d,UINT w,UINT h,UINT depth,UINT levels,DWORD usage,D3DFORMAT f,D3DPOOL p,IDirect3DVolumeTexture9** out,HANDLE* shared){
    auto hr=g_volume(d,w,h,depth,levels,usage,f,p,out,shared);if(SUCCEEDED(hr)&&out&&*out){uint64_t bytes=0;
        for(UINT i=0;i<(*out)->GetLevelCount();++i){D3DVOLUME_DESC desc{};if(SUCCEEDED((*out)->GetLevelDesc(i,&desc)))bytes+=LevelBytes(desc.Width,desc.Height,desc.Depth,desc.Format);}
        Attach(*out,Volume,p,bytes);
    }else if(FAILED(hr))++g_createFailed;return hr;
}
HRESULT STDMETHODCALLTYPE CubeHook(Device* d,UINT edge,UINT levels,DWORD usage,D3DFORMAT f,D3DPOOL p,IDirect3DCubeTexture9** out,HANDLE* shared){
    auto hr=g_cube(d,edge,levels,usage,f,p,out,shared);if(SUCCEEDED(hr)&&out&&*out){uint64_t bytes=0;
        for(UINT i=0;i<(*out)->GetLevelCount();++i){D3DSURFACE_DESC desc{};if(SUCCEEDED((*out)->GetLevelDesc(i,&desc)))bytes+=6*LevelBytes(desc.Width,desc.Height,1,desc.Format);}
        Attach(*out,Cube,p,bytes);
    }else if(FAILED(hr))++g_createFailed;return hr;
}
HRESULT STDMETHODCALLTYPE VertexHook(Device* d,UINT n,DWORD usage,DWORD fvf,D3DPOOL p,IDirect3DVertexBuffer9** out,HANDLE* shared){auto hr=g_vertex(d,n,usage,fvf,p,out,shared);if(SUCCEEDED(hr)&&out&&*out)Attach(*out,Vertex,p,n);else if(FAILED(hr))++g_createFailed;return hr;}
HRESULT STDMETHODCALLTYPE IndexHook(Device* d,UINT n,DWORD usage,D3DFORMAT f,D3DPOOL p,IDirect3DIndexBuffer9** out,HANDLE* shared){auto hr=g_index(d,n,usage,f,p,out,shared);if(SUCCEEDED(hr)&&out&&*out)Attach(*out,Index,p,n);else if(FAILED(hr))++g_createFailed;return hr;}
HRESULT STDMETHODCALLTYPE TargetHook(Device* d,UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE m,DWORD q,BOOL lock,IDirect3DSurface9** out,HANDLE* shared){auto hr=g_target(d,w,h,f,m,q,lock,out,shared);if(SUCCEEDED(hr)&&out&&*out)Attach(*out,RenderTarget,D3DPOOL_DEFAULT,LevelBytes(w,h,1,f));else if(FAILED(hr))++g_createFailed;return hr;}
HRESULT STDMETHODCALLTYPE DepthHook(Device* d,UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE m,DWORD q,BOOL discard,IDirect3DSurface9** out,HANDLE* shared){auto hr=g_depth(d,w,h,f,m,q,discard,out,shared);if(SUCCEEDED(hr)&&out&&*out)Attach(*out,DepthSurface,D3DPOOL_DEFAULT,LevelBytes(w,h,1,f));else if(FAILED(hr))++g_createFailed;return hr;}
HRESULT STDMETHODCALLTYPE OffscreenHook(Device* d,UINT w,UINT h,D3DFORMAT f,D3DPOOL p,IDirect3DSurface9** out,HANDLE* shared){auto hr=g_offscreen(d,w,h,f,p,out,shared);if(SUCCEEDED(hr)&&out&&*out)Attach(*out,Offscreen,p,LevelBytes(w,h,1,f));else if(FAILED(hr))++g_createFailed;return hr;}
template<class Fn>void Swap(void** v,unsigned index,Fn hook,Fn& original){if(v[index]==reinterpret_cast<void*>(hook))return;original=reinterpret_cast<Fn>(v[index]);void* p=reinterpret_cast<void*>(hook);wxl::mem::Patch(&v[index],&p,sizeof p);}
void Ensure(void*,const void*){
    auto* d=static_cast<Device*>(wxl::game::gx::RawDevice());if(!d)return;auto** v=*reinterpret_cast<void***>(d);
    Swap(v,23,&TextureHook,g_texture);Swap(v,24,&VolumeHook,g_volume);Swap(v,25,&CubeHook,g_cube);
    Swap(v,26,&VertexHook,g_vertex);Swap(v,27,&IndexHook,g_index);Swap(v,28,&TargetHook,g_target);Swap(v,29,&DepthHook,g_depth);Swap(v,36,&OffscreenHook,g_offscreen);
    Report();
}
bool Install(){if(!wxl::diag::MemoryOwnersEnabled())return true;Ensure(nullptr,nullptr);wxl::events::Subscribe(wxl::events::Event::OnUpdate,&Ensure,nullptr);
    WLOG_INFO("gpu-memory-v1: lifetime counters active; payload estimates exclude driver copies, padding, MSAA and pre-hook resources");return true;}
}
WXL_REGISTER_FEATURE("gpu-resource-diagnostic",true,Install)
