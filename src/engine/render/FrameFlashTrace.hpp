// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <array>
namespace wxl::render::flashtrace {
using Clock=std::chrono::steady_clock;
inline double Ms(Clock::duration d) { return std::chrono::duration<double,std::milli>(d).count(); }
struct Pending {
    Microsoft::WRL::ComPtr<IDirect3DSurface9> reduced,read;
    Microsoft::WRL::ComPtr<IDirect3DQuery9> ready;
    D3DFORMAT format=D3DFMT_UNKNOWN;
    uint64_t frame=0,generation=0,elapsed=0;
    unsigned stage=0,effects=0;
    bool queued=false;
};
struct State {
    FILE* file=nullptr; bool initialized=false,armed=false,effects=false;
    uint64_t frame=0,generation=0; unsigned mask=0,samples=0,slow=0;
    ULONGLONG start=0,next=0; double cost=0;
    Clock::time_point lastPresent{};
    std::array<Pending,30> pending;
    bool Enabled() {
        if(!initialized) {
            initialized=true; wchar_t path[1200]{};
            auto size=GetEnvironmentVariableW(L"WXL_FRAME_FLASH_TRACE",path,1200);
            if(size && size<1200 && _wfopen_s(&file,path,L"wx")==0 && file) {
                fprintf(file,"frame,generation,elapsed_ms,stage,effects,mean_luma,dark_fraction,capture_ms,frame_ms,hr\n");
                fflush(file);
            }
        }
        return file!=nullptr;
    }
    void Release() { for(auto& p:pending) p=Pending{}; armed=false; }
    void Stop() { Release(); if(file) { fclose(file); file=nullptr; } }
};
inline State& S() { static auto* state=new State; return *state; }
inline void Begin(uint64_t frame,uint64_t generation,bool effects) {
    auto& s=S(); if(!s.Enabled()) return;
    const auto now=GetTickCount64(); if(!s.start) s.start=now;
    if(now-s.start>180000) { s.Stop(); return; }
    if(s.generation!=generation) s.Release();
    s.frame=frame; s.generation=generation; s.effects=effects; s.mask=0; s.cost=0;
    s.armed=now>=s.next;
    if(s.armed) { s.next=now+33; ++s.samples; }
}
inline void Sample(IDirect3DDevice9* d,unsigned stage) {
    auto& s=S(); if(!s.file || !s.armed || (stage>5 || stage==4) || (s.mask&(1u<<stage))) return;
    s.mask|=1u<<stage;
    const auto start=Clock::now();
    Microsoft::WRL::ComPtr<IDirect3DSurface9> source;
    D3DSURFACE_DESC desc{};
    HRESULT hr=stage==3 ? d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&source) : d->GetRenderTarget(0,&source);
    if(SUCCEEDED(hr) && source) hr=source->GetDesc(&desc);
    else hr=E_FAIL;
    // Diagnostic targets are independent and never bound to the device. Skip MSAA:
    // downsampling its resolve requires a full-size intermediate and greater cost.
    if(SUCCEEDED(hr) && (desc.MultiSampleType!=D3DMULTISAMPLE_NONE ||
        (desc.Format!=D3DFMT_X8R8G8B8 && desc.Format!=D3DFMT_A8R8G8B8))) hr=D3DERR_NOTAVAILABLE;
    Pending* slot=nullptr;
    for(auto& p:s.pending) if(!p.queued) { slot=&p; break; }
    if(SUCCEEDED(hr) && !slot) hr=D3DERR_WASSTILLDRAWING;
    if(SUCCEEDED(hr)) {
        auto& p=*slot;
        if(!p.reduced || !p.read || !p.ready || p.format!=desc.Format) {
            p=Pending{}; p.format=desc.Format;
            hr=d->CreateRenderTarget(32,18,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&p.reduced,nullptr);
            if(SUCCEEDED(hr)) hr=d->CreateOffscreenPlainSurface(32,18,desc.Format,D3DPOOL_SYSTEMMEM,&p.read,nullptr);
            if(SUCCEEDED(hr)) hr=d->CreateQuery(D3DQUERYTYPE_EVENT,&p.ready);
        }
        if(SUCCEEDED(hr)) hr=d->StretchRect(source.Get(),nullptr,p.reduced.Get(),nullptr,D3DTEXF_LINEAR);
        if(SUCCEEDED(hr)) hr=p.ready->Issue(D3DISSUE_END);
        if(SUCCEEDED(hr)) {
            p.frame=s.frame; p.generation=s.generation; p.elapsed=GetTickCount64()-s.start;
            p.stage=stage; p.effects=s.effects; p.queued=true;
        }
    }
    const double cost=Ms(Clock::now()-start); s.cost+=cost;
    if(FAILED(hr)) fprintf(s.file,"%llu,%llu,%llu,%u,%u,-1,-1,%.3f,0,%08lX\n",s.frame,s.generation,GetTickCount64()-s.start,stage,s.effects,cost,hr);
}
inline void Collect(IDirect3DDevice9* d) {
    auto& s=S(); if(!s.file || !d) return;
    const auto start=Clock::now();
    for(auto& p:s.pending) {
        if(!p.queued) continue;
        // Present submits normal work. Never flush or wait for an unfinished query.
        HRESULT hr=p.ready->GetData(nullptr,0,0);
        if(hr==S_FALSE) continue;
        const auto readStart=Clock::now();
        double mean=-1,dark=-1;
        if(SUCCEEDED(hr)) hr=d->GetRenderTargetData(p.reduced.Get(),p.read.Get());
        D3DLOCKED_RECT lock{};
        if(SUCCEEDED(hr)) hr=p.read->LockRect(&lock,nullptr,D3DLOCK_READONLY);
        if(SUCCEEDED(hr)) {
            mean=dark=0;
            for(unsigned y=0;y<18;++y) for(unsigned x=0;x<32;++x) {
                auto pixel=reinterpret_cast<const uint32_t*>(static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x];
                const double luma=(.2126*((pixel>>16)&255)+.7152*((pixel>>8)&255)+.0722*(pixel&255))/255;
                mean+=luma; dark+=luma<.04;
            }
            p.read->UnlockRect(); mean/=576; dark/=576;
        }
        fprintf(s.file,"%llu,%llu,%llu,%u,%u,%.6f,%.6f,%.3f,0,%08lX\n",p.frame,p.generation,p.elapsed,p.stage,p.effects,mean,dark,Ms(Clock::now()-readStart),hr);
        p.queued=false;
    }
    s.cost+=Ms(Clock::now()-start);
}
inline void Presented(HRESULT hr,IDirect3DDevice9* d) {
    auto& s=S(); if(!s.file) return;
    Collect(d);
    const auto now=Clock::now();
    const double gap=s.lastPresent==Clock::time_point{} ? 0 : Ms(now-s.lastPresent); s.lastPresent=now;
    if(!s.start) return;
    if(GetTickCount64()-s.start>180000) { s.Stop(); return; }
    fprintf(s.file,"%llu,%llu,%llu,4,%u,-1,-1,%.3f,%.3f,%08lX\n",s.frame,s.generation,GetTickCount64()-s.start,s.effects,s.cost,gap,hr);
    if(s.cost>0) {
        fflush(s.file);
        if(GetTickCount64()-s.start>15000 && s.cost>8) ++s.slow;
        if(s.slow>=3) { fprintf(s.file,"# stopped: three post-warmup frames exceeded 8 ms monitoring cost\n"); s.Stop(); }
    }
    s.armed=false;
}
inline void Lost() { S().Release(); }
}
