// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderService.hpp"
#include "wxl/RenderDefaults.hpp"
#include "FramePipeline.hpp"
#include "SceneColor.hpp"
#include "DepthService.hpp"
#include "RenderPerformance.hpp"
#include "ComparisonState.hpp"
#include "FrameFlashTrace.hpp"
#include "engine/events/Event.hpp"
namespace wxl::render
{
    namespace
    {
        ComparisonState comparison;
        int __cdecl EffectsEnabled() { return comparison.Enabled() ? 1 : 0; }
        const WXL_RenderControlApi controlApi{sizeof(WXL_RenderControlApi),WXL_RENDER_CONTROL_API_VERSION,EffectsEnabled};
        FramePipeline<SceneColor>& Pipeline()
        {
            // Process lifetime, with GPU cleanup before Reset. No driver calls from a global
            // destructor under the DLL loader lock.
            static auto* pipeline = new FramePipeline<SceneColor>;
            return *pipeline;
        }
        int __cdecl AddPostProcess(uint32_t stage, WXL_RenderPassFn fn, void* user)
        { return Pipeline().AddPostProcess(stage, fn, user); }
        void __cdecl RequestSceneColor() { Pipeline().RequestSceneColor(); }
        int __cdecl GetSceneColor(WXL_SceneColor* out) { return Pipeline().GetSceneColor(out); }
        const WXL_RenderApi kApi{sizeof(WXL_RenderApi), WXL_RENDER_API_VERSION,
            sizeof(WXL_RenderPassContext), sizeof(WXL_SceneColor),
            AddPostProcess, RequestSceneColor, GetSceneColor};
    }
    const WXL_RenderApi* Api() { return &kApi; }
    const WXL_RenderControlApi* ControlApi() { return &controlApi; }
    void OnComparisonInput(void*,const void* args)
    {
        static const bool available=wxl::render::defaults::Enabled("WXL_RENDER_COMPARE");
        if(!available || !args) return;
        const auto& input=*static_cast<const wxl::events::InputArgs*>(args);
        if(input.handled && *input.handled) return;
        if(comparison.Input(input.message,input.wparam,input.lparam) && input.handled) *input.handled=true;
    }
    void BeginWorld(void* device)
    {
        if(device && comparison.BeginWorld()) WLOG_INFO("render-comparison: %s (F11)",comparison.Enabled() ? "enhancements restored" : "comparison baseline");
        Pipeline().BeginWorld(device);
        depth::Begin(device, Pipeline().FrameId(), Pipeline().DeviceGeneration());
        flashtrace::Begin(Pipeline().FrameId(),Pipeline().DeviceGeneration(),comparison.Enabled());
    }
    void NativeWorldBegin(void* device)
    {
        if(!Pipeline().CanDispatch(device)) return;
        const WXL_RenderPassContext ctx{sizeof(ctx),0,Pipeline().FrameId(),Pipeline().DeviceGeneration(),device};
        wxl::events::Emit(wxl::events::Event::OnNativeWorldBegin,&ctx);
    }
    void BeforeLiquids(int passType, uint32_t batchCount, bool mainWorld)
    {
        if (passType < 0 || passType > 1) return;
        ++perf::liquidCalls[passType];
        if (batchCount) ++perf::nonemptyLiquids[passType];
        if (!mainWorld) { ++perf::outsideWorld; return; }
        if (!Pipeline().BeforeLiquids(passType, batchCount)) return;
        { perf::Scope timing(perf::Stage::WaterDepthCopy); depth::CaptureWaterDepth(); }
        static unsigned reports = 0;
        if (reports++ < 3)
        {
            WXL_SceneColor color{}; color.structSize = sizeof(color);
            const bool ready = Pipeline().GetSceneColor(&color) != 0;
            WLOG_INFO("render-color: pass=%d batches=%u ready=%u frame=%llu size=%ux%u",
                passType, batchCount, ready, Pipeline().FrameId(), color.width, color.height);
        }
    }
    void BeforeTransparents(void* device)
    {
        if(!Pipeline().CanDispatchTransparent(device)) return;
        const auto frame=Pipeline().FrameId(),generation=Pipeline().DeviceGeneration();
        depth::SaveCamera(); depth::BeginRead();
        Pipeline().DispatchTransparent(device,comparison.Enabled());
        if(Pipeline().FrameId()==frame && Pipeline().DeviceGeneration()==generation) depth::EndRead();
    }
    void BeforeScreenEffects(void* device)
    {
        if(!Pipeline().CanDispatchEarly(device)) return;
        const auto frame=Pipeline().FrameId(),generation=Pipeline().DeviceGeneration();
        depth::SaveCamera();
        depth::BeginRead();
        flashtrace::Sample(static_cast<IDirect3DDevice9*>(device),0);
        Pipeline().DispatchEarly(device,comparison.Enabled());
        if(Pipeline().FrameId()==frame && Pipeline().DeviceGeneration()==generation) flashtrace::Sample(static_cast<IDirect3DDevice9*>(device),1);
        // A reset/world transition already retired the old capture. Do not close
        // a newly created frame's read interval using the cancelled frame's scope.
        if(Pipeline().FrameId()==frame && Pipeline().DeviceGeneration()==generation) depth::EndRead();
    }
    void Dispatch(void* device)
    {
        // Apply the same guard to the depth read interval and preview as to callbacks.
        // A callback may reenter this boundary; it must not close the outer read interval.
        if (!Pipeline().CanDispatch(device)) return;
        const auto frame = Pipeline().FrameId(), generation = Pipeline().DeviceGeneration();
        { perf::Scope timing(perf::Stage::DepthRead); depth::BeginRead(); }
        flashtrace::Sample(static_cast<IDirect3DDevice9*>(device),5); // After native effects, before WXL late passes.
        { perf::Scope timing(perf::Stage::PostProcess); Pipeline().Dispatch(device,comparison.Enabled()); }
        if (Pipeline().FrameId() == frame && Pipeline().DeviceGeneration() == generation)
        {
            perf::Scope timing(perf::Stage::Preview);
            depth::Preview();
        }
        { perf::Scope timing(perf::Stage::DepthRead); depth::EndRead(); }
        if(Pipeline().FrameId()==frame && Pipeline().DeviceGeneration()==generation) flashtrace::Sample(static_cast<IDirect3DDevice9*>(device),2);
        perf::Frame();
    }
    void EndWorld() { depth::End(); Pipeline().EndWorld(); }
    void DeviceLost() { flashtrace::Lost(); depth::Reset(); Pipeline().DeviceLost(); }
}
