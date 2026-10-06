#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace wxl::diag {
struct AllocationSite {
    uintptr_t caller = 0, tag = 0;
    uint32_t line = 0;
    uintptr_t parent = 0;
    bool operator==(const AllocationSite&) const = default;
};
struct AllocationRecord { uintptr_t pointer = 0; uint32_t bytes = 0; AllocationSite site{}; };
// Fixed storage; caller provides synchronization. This ledger never owns payloads.
template<size_t N> class AllocationLedger {
    static_assert(N && !(N & (N-1)));
    std::array<AllocationRecord,N> slots_{};
    static size_t Hash(uintptr_t p) { return ((p >> 4)*uintptr_t(2654435761u)) & (N-1); }
public:
    uint64_t bytes=0, missed=0;
    size_t count=0;
    bool Add(AllocationRecord record) noexcept {
        if(record.pointer<=1)return false;
        size_t reuse=N;
        for(size_t i=0;i<N;++i){
            const size_t at=(Hash(record.pointer)+i)&(N-1);auto& s=slots_[at];
            if(s.pointer==record.pointer){bytes-=s.bytes;s=record;bytes+=s.bytes;return true;}
            if(s.pointer==1 && reuse==N)reuse=at;
            if(!s.pointer){if(reuse==N)reuse=at;break;}
        }
        if(reuse==N){++missed;return false;}
        slots_[reuse]=record;bytes+=record.bytes;++count;return true;
    }
    AllocationRecord Remove(uintptr_t p) noexcept {
        if(p<=1)return {};
        for(size_t i=0;i<N;++i){auto& s=slots_[(Hash(p)+i)&(N-1)];
            if(!s.pointer)return {};
            if(s.pointer==p){auto old=s;bytes-=s.bytes;--count;s={1,0,{}};return old;}
        }return {};
    }
    template<class F> void Each(F fn) const {for(const auto& s:slots_)if(s.pointer>1)fn(s);}
};
}
