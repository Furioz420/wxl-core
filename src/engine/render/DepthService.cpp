// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#include "DepthService.hpp"
#include "wxl/RenderDefaults.hpp"
#include "DepthCapture.hpp"
#include "DepthCamera.hpp"
#include "DepthPreview.hpp"
#include "DepthSupport.hpp"
#include "WaterDepthCopy.hpp"
#include "PreviewHotkey.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/Gx.hpp"
#include <algorithm>
#include <cstring>

namespace wxl::render::depth
{
    namespace
    {
        using Capture = DepthCapture<IDirect3DDevice9, IDirect3DTexture9, IDirect3DSurface9>;
        struct State
        {
            Capture capture;
            WaterDepthCopy waterCopy, liquidCopy;
            WXL_SceneDepth liquidSnapshot{};
            bool liquidRequested=false, liquidReady=false;
            WXL_SceneDepth waterSnapshot{};
            Microsoft::WRL::ComPtr<IDirect3DPixelShader9> waterShader;
            bool waterRequested=false, waterReady=false;
            unsigned waterReports=0;
            WXL_SceneDepth snapshot{};
            Microsoft::WRL::ComPtr<IDirect3DPixelShader9> previewShader;
            IDirect3DDevice9* device = nullptr;
            bool requested = false, cameraValid = false, preview = false;
            PreviewHotkey hotkey;
            unsigned reports = 0;
            unsigned clearTraces=0; bool traceClearResult=false;
            int lastStatus = -1;
            bool previewReported = false;
            State()
            {
                const unsigned mode=wxl::render::defaults::Read("WXL_RENDER_DEPTH",true);
                requested=mode!=0; preview=mode==2;
                if(requested) WLOG_INFO("render-depth: enabled; preview=%u; F8 toggles preview",preview);
            }
        };
        State& S() { static auto* s = new State; return *s; }
        void HandleInput(void*, const void* args)
        {
            const auto* a = static_cast<const events::InputArgs*>(args);
            auto& s = S();
            const bool before = s.preview;
            if (s.requested && s.hotkey.Handle(a->message, a->wparam, s.preview))
                *a->handled = true;
            if (before != s.preview)
                WLOG_INFO("render-depth: preview %s (F8)", s.preview ? "shown" : "hidden");
        }
        void __cdecl Request() { if(wxl::render::defaults::Read("WXL_RENDER_DEPTH",true)!=0) S().requested = true; }
        int __cdecl GetSnapshot(WXL_SceneDepth* out)
        {
            if (!out || out->structSize != sizeof(*out)) return 0;
            *out = {}; out->structSize = sizeof(*out);
            auto& s = S();
            if (!s.cameraValid || !s.capture.ReadTexture()) return 0;
            *out = s.snapshot;
            out->texture = s.capture.ReadTexture();
            return 1;
        }
        void __cdecl RequestMsaa() { Request(); S().capture.allowMultisampled=true; }
        int __cdecl GetMsaa(WXL_MultisampleDepth* out) {
            if(!out || out->structSize!=sizeof(*out)) return 0;
            *out={}; out->structSize=sizeof(*out);
            auto& s=S(); auto* surface=s.capture.ReadMultisampledSurface();
            if(!s.cameraValid || !surface) return 0;
            D3DSURFACE_DESC desc{}; if(FAILED(surface->GetDesc(&desc))) return 0;
            out->frame=s.snapshot; out->frame.texture=nullptr; out->frame.format=desc.Format;
            out->surface=surface; return 1;
        }
        const WXL_MultisampleDepthApi msaaApi{sizeof(msaaApi),WXL_MULTISAMPLE_DEPTH_API_VERSION,RequestMsaa,GetMsaa};
        const WXL_RenderDepthApi kApi{sizeof(kApi), WXL_RENDER_DEPTH_API_VERSION,
            sizeof(WXL_SceneDepth), Request, GetSnapshot};
        void __cdecl RequestWater() { S().waterRequested=true; }
        int __cdecl GetWater(WXL_SceneDepth* out)
        {
            if (!out || out->structSize!=sizeof(*out)) return 0;
            *out={}; out->structSize=sizeof(*out);
            auto& s=S();
            if (!s.waterReady || !s.capture.Valid()) return 0;
            *out=s.waterSnapshot;
            return 1;
        }
        const WXL_WaterDepthApi kWaterApi{sizeof(kWaterApi),WXL_WATER_DEPTH_API_VERSION,
            sizeof(WXL_SceneDepth),RequestWater,GetWater};
        void __cdecl RequestLiquid() { S().liquidRequested=true; S().waterRequested=true; }
        int __cdecl GetLiquid(WXL_SceneDepth* out) {
            if(!out || out->structSize!=sizeof(*out)) return 0;
            *out={}; out->structSize=sizeof(*out);
            if(!S().liquidReady || !S().capture.Valid()) return 0;
            *out=S().liquidSnapshot; return 1;
        }
        const WXL_LiquidDepthApi kLiquidApi{sizeof(kLiquidApi),WXL_LIQUID_DEPTH_API_VERSION,
            sizeof(WXL_SceneDepth),RequestLiquid,GetLiquid};
        const char* StatusName(DepthStatus status)
        {
            switch (status)
            {
            case DepthStatus::Waiting: return "no eligible world depth clear";
            case DepthStatus::Ready: return "ready";
            case DepthStatus::Multisampled: return "MSAA unsupported; native depth retained";
            case DepthStatus::Format: return "unsupported format or dimensions";
            case DepthStatus::Viewport: return "unsupported viewport";
            case DepthStatus::Target: return "not the backbuffer world target";
            case DepthStatus::Allocation: return "INTZ allocation failed";
            case DepthStatus::Bind: return "INTZ bind failed";
            case DepthStatus::ClearFailed: return "INTZ clear failed; retry on native depth";
            case DepthStatus::ClearedAgain: return "depth cleared again; snapshot invalid";
            case DepthStatus::ReadFailed: return "depth could not be detached for reading";
            case DepthStatus::RestoreFailed: return "depth restore failed";
            case DepthStatus::Camera: return "world camera invalid";
            case DepthStatus::Capability: return "INTZ capability unsupported";
            case DepthStatus::Stencil: return "stencil contents must be preserved";
            }
            return "unknown";
        }
        void Report(DepthStatus status)
        {
            auto& s = S();
            if (static_cast<int>(status) == s.lastStatus || s.reports >= 24) return;
            s.lastStatus = static_cast<int>(status); ++s.reports;
            WLOG_INFO("render-depth: %s; frame=%llu size=%ux%u generation=%llu",
                StatusName(status), s.snapshot.frameId, s.capture.Width(), s.capture.Height(),
                s.snapshot.deviceGeneration);
            const auto& v = s.capture.Viewport();
            const auto& c = s.capture.ColorDesc();
            const auto& d = s.capture.DepthDesc();
            WLOG_INFO("render-depth-surfaces: color=%ux%u fmt=%u aa=%u quality=%u depth=%ux%u fmt=%u aa=%u quality=%u",
                c.Width,c.Height,c.Format,c.MultiSampleType,c.MultiSampleQuality,
                d.Width,d.Height,d.Format,d.MultiSampleType,d.MultiSampleQuality);
            WLOG_INFO("render-depth: target=%ux%u format=%u viewport=%u,%u,%u,%u depthRange=%.9g..%.9g",
                c.Width, c.Height, static_cast<unsigned>(c.Format), v.X, v.Y, v.Width, v.Height, v.MinZ, v.MaxZ);
        }
    }
    const WXL_MultisampleDepthApi* MultisampleApi() { return &msaaApi; }
    const WXL_RenderDepthApi* Api() { return &kApi; }
    const WXL_WaterDepthApi* WaterApi() { return &kWaterApi; }
    const WXL_LiquidDepthApi* LiquidApi() { return &kLiquidApi; }
    void BeginLiquidRead() { if(S().capture.Multisampled() && S().cameraValid) S().capture.BeginRead(); }
    void DiscardLiquidDepth() { if(S().capture.Multisampled()) S().capture.EndRead(); S().liquidReady=false; }
    void CaptureWaterDepth(bool after)
    {
        auto& s=S();
        if(after && !s.liquidRequested) return;
        bool& ready=after ? s.liquidReady : s.waterReady;
        auto& copy=after ? s.liquidCopy : s.waterCopy;
        auto& snapshot=after ? s.liquidSnapshot : s.waterSnapshot;
        ready=false;
        if (!s.waterRequested || !s.requested || !s.capture.Valid()) return;
        SaveCamera();
        if (!s.cameraValid || s.capture.Multisampled()) return;
        if (!s.waterShader)
            s.waterShader.Attach(static_cast<IDirect3DPixelShader9*>(game::gx::CompilePixelShader(
                game::gx::Device9(s.device),kWaterDepthCopyShader,"ps_3_0")));
        if (!s.waterShader || !s.capture.BeginRead()) return;
        const bool copied=copy.Copy(s.device,s.capture.ReadTexture(),s.waterShader.Get(),
            s.snapshot.width,s.snapshot.height);
        const bool restored=s.capture.EndRead();
        ready=copied && restored;
        if (ready)
        {
            snapshot=s.snapshot;
            snapshot.texture=copy.Texture();
            snapshot.format=D3DFMT_R32F;
        }
        if (s.waterReports++<4)
            WLOG_INFO("render-water-depth: ready=%u restored=%u frame=%llu size=%ux%u",
                ready,restored,s.snapshot.frameId,s.snapshot.width,s.snapshot.height);
    }
    void OnInput(void* user, const void* args) { HandleInput(user, args); }
    void Configure(GetFn get, SetFn set)
    {
        S().capture.get = get; S().capture.set = set; S().capture.supports = SupportsIntz;
    }
    void Begin(void* device, uint64_t frame, uint64_t generation)
    {
        auto& s = S();
        s.cameraValid = false; s.snapshot = {};
        s.waterReady=false; s.liquidReady=false;
        s.snapshot.structSize = sizeof(s.snapshot);
        s.snapshot.frameId = frame; s.snapshot.deviceGeneration = generation;
        if (device != s.device) { s.previewShader.Reset(); s.waterShader.Reset(); s.liquidCopy.Reset(); s.waterCopy.Reset(); }
        s.device = static_cast<IDirect3DDevice9*>(device);
        if (s.requested) s.capture.Begin(s.device);
    }
    void BeforeClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, float z, bool mainWorld, void* caller)
    {
        auto& s=S(); auto& c=s.capture;
        if(!c.Owns(device)) return;
        const bool wasValid=c.Valid();
        c.BeforeClear(count,rects,flags,z,mainWorld);
        static const bool trace=[] { char value[2]{}; return GetEnvironmentVariableA("WXL_RENDER_DEPTH_TRACE",value,2)==1 && value[0]=='1'; }();
        if(!trace || !wasValid || c.Valid() || !c.Multisampled() || s.clearTraces>=12) return;
        ++s.clearTraces; s.traceClearResult=true;
        Microsoft::WRL::ComPtr<IDirect3DSurface9> bound,target;
        const HRESULT binding=c.get(device,&bound);
        D3DSURFACE_DESC dd{},rd{}; D3DVIEWPORT9 vp{};
        if(bound) bound->GetDesc(&dd);
        if(SUCCEEDED(device->GetRenderTarget(0,&target)) && target) target->GetDesc(&rd);
        device->GetViewport(&vp);
        WLOG_INFO("depth-clear-trace: n=%u frame=%llu caller=%p flags=0x%lX count=%lu z=%.9g world=%u binding=0x%08lX surface=%p target=%p",
            s.clearTraces,s.snapshot.frameId,caller,flags,count,z,mainWorld,binding,bound.Get(),target.Get());
        WLOG_INFO("depth-clear-trace: depth=%ux%u fmt=%u aa=%u quality=%u target=%ux%u fmt=%u aa=%u viewport=%u,%u,%u,%u",
            dd.Width,dd.Height,dd.Format,dd.MultiSampleType,dd.MultiSampleQuality,rd.Width,rd.Height,rd.Format,rd.MultiSampleType,vp.X,vp.Y,vp.Width,vp.Height);
        if(count && rects) WLOG_INFO("depth-clear-trace: first-rect=%ld,%ld,%ld,%ld",rects[0].x1,rects[0].y1,rects[0].x2,rects[0].y2);
        void* stack[16]{}; const USHORT frames=CaptureStackBackTrace(0,16,stack,nullptr);
        for(USHORT i=0;i<frames;++i) {
            HMODULE module=nullptr; char path[MAX_PATH]{};
            if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(stack[i]),&module)) GetModuleFileNameA(module,path,MAX_PATH);
            const char* name=std::strrchr(path,'\\'); name=name ? name+1 : path;
            WLOG_INFO("depth-clear-trace: stack[%u]=%p %s+0x%lX",i,stack[i],name,
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(stack[i])-reinterpret_cast<uintptr_t>(module)));
        }
    }
    bool AfterClear(IDirect3DDevice9* device, HRESULT result)
    {
        auto& s=S(); auto& c=s.capture;
        const bool retry=c.Owns(device) && c.AfterClear(result);
        if(s.traceClearResult) {
            s.traceClearResult=false;
            WLOG_INFO("depth-clear-trace: clear-result=0x%08lX retry=%u",result,retry);
        }
        return retry;
    }
    HRESULT Set(IDirect3DDevice9* device, IDirect3DSurface9* surface)
    { auto& c = S().capture; return c.Owns(device) ? c.Set(surface) : c.set(device, surface); }
    HRESULT Get(IDirect3DDevice9* device, IDirect3DSurface9** out)
    { auto& c = S().capture; return c.Owns(device) ? c.Get(out) : c.get(device, out); }
    void SaveCamera()
    {
        auto& s = S();
        if (!s.requested || !s.capture.Valid()) return;
        std::copy_n(game::camera::GetView(), 16, s.snapshot.view);
        std::copy_n(game::camera::GetProjection(), 16, s.snapshot.projection);
        game::camera::GetPosition(s.snapshot.cameraPosition);
        s.cameraValid = ValidDepthCamera(s.snapshot.view, s.snapshot.projection, s.snapshot.cameraPosition);
        s.snapshot.width = s.capture.Width(); s.snapshot.height = s.capture.Height();
        s.snapshot.format = static_cast<uint32_t>(Capture::Format);
        s.snapshot.depthMinZ = s.capture.Viewport().MinZ;
        s.snapshot.depthMaxZ = s.capture.Viewport().MaxZ;
    }
    void BeginRead()
    {
        auto& s = S();
        if (!s.requested) return;
        if (s.cameraValid) s.capture.BeginRead();
        Report(s.capture.Valid() && !s.cameraValid ? DepthStatus::Camera : s.capture.Status());
    }
    void Preview()
    {
        auto& s = S();
        WXL_SceneDepth input{}; input.structSize = sizeof(input);
        if (!s.preview || !GetSnapshot(&input)) return;
        auto* d = s.device;
        D3DVIEWPORT9 vp{};
        if (!d || FAILED(d->GetViewport(&vp)) || vp.X || vp.Y ||
            vp.Width != input.width || vp.Height != input.height) return;
        Microsoft::WRL::ComPtr<IDirect3DSurface9> target, back;
        if (FAILED(d->GetRenderTarget(0, target.GetAddressOf())) ||
            FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.GetAddressOf())) ||
            target.Get() != back.Get()) return;
        for (DWORD slot = 1; slot < 4; ++slot)
        {
            Microsoft::WRL::ComPtr<IDirect3DSurface9> extra;
            if (SUCCEEDED(d->GetRenderTarget(slot, extra.GetAddressOf())) && extra) return;
        }
        if (!s.previewShader)
        {
            // Raw D3D depth -> forward view distance. White at 100 world units and beyond.
            s.previewShader.Attach(static_cast<IDirect3DPixelShader9*>(
                game::gx::CompilePixelShader(game::gx::Device9(d), kDepthPreviewShader, "ps_3_0")));
            if (!s.previewShader) { s.preview = false; WLOG_WARN("render-depth: preview shader unavailable"); return; }
        }
        Microsoft::WRL::ComPtr<IDirect3DStateBlock9> state;
        if (FAILED(d->CreateStateBlock(D3DSBT_ALL, state.GetAddressOf())) ||
            FAILED(state->Capture())) return;
        d->SetVertexShader(nullptr); d->SetPixelShader(s.previewShader.Get());
        d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_STENCILENABLE, FALSE); d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE); d->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
        d->SetTexture(0, static_cast<IDirect3DTexture9*>(input.texture));
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        const float constants[4]{input.projection[10], input.projection[14], input.depthMinZ,
            1.0f / (input.depthMaxZ - input.depthMinZ)};
        d->SetPixelShaderConstantF(0, constants, 1);
        struct V { float x,y,z,w,u,v; };
        const float w = std::min(384.0f, static_cast<float>(vp.Width) * 0.4f);
        const float h = w * vp.Height / vp.Width;
        const V quad[]{{15.5f,15.5f,0,1,0,0},{15.5f+w,15.5f,0,1,1,0},
            {15.5f,15.5f+h,0,1,0,1},{15.5f+w,15.5f+h,0,1,1,1}};
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        const HRESULT drawn = d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        const HRESULT restored = state->Apply();
        if (!s.previewReported)
        {
            s.previewReported = true;
            WLOG_INFO("render-depth: preview draw hr=0x%08lX restore=0x%08lX range=%.9g..%.9g",
                static_cast<unsigned long>(drawn), static_cast<unsigned long>(restored), input.depthMinZ, input.depthMaxZ);
        }
    }
    void EndRead() { if (!S().capture.EndRead()) Report(DepthStatus::RestoreFailed); }
    void End()
    {
        auto& s = S(); s.cameraValid = false; s.waterReady=false; s.liquidReady=false;
        if (!s.capture.End()) Report(DepthStatus::RestoreFailed);
    }
    void Reset()
    {
        auto& s = S(); s.capture.Reset(); s.previewShader.Reset();
        s.liquidCopy.Reset(); s.waterCopy.Reset(); s.waterShader.Reset(); s.waterReady=false; s.liquidReady=false; s.waterReports=0;
        s.cameraValid = false; s.device = nullptr; s.lastStatus = -1;
        s.previewReported = false;
    }
}
