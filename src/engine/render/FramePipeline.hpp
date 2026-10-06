// World-frame lifetime and ordered dispatch. Copyright (C) 2026 WarcraftXL.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "wxl/RenderApi.h"
#include <algorithm>
#include <vector>

namespace wxl::render
{
    // ColorSource implements Capture(device, out) and Release(). Tests exercise this actual
    // lifetime/dispatch implementation with a fake GPU source.
    template<class ColorSource> class FramePipeline
    {
    public:
        ColorSource source;
        uint64_t FrameId() const { return frame_; }
        uint64_t DeviceGeneration() const { return generation_; }
        bool CanDispatch(void* device) const
        { return active_ && !dispatched_ && !dispatching_ && device == device_; }
        int AddPostProcess(uint32_t stage, WXL_RenderPassFn fn, void* user)
        {
            if (dispatching_ || !fn || !ValidStage(stage)) return 0;
            for (const auto& pass : passes_)
                if (pass.fn == fn && pass.user == user) return 0;
            const auto where = std::upper_bound(passes_.begin(), passes_.end(), stage,
                [](uint32_t value, const Pass& p) { return value < p.stage; });
            passes_.insert(where, {stage, fn, user});
            return 1;
        }
        void RequestSceneColor() { requested_ = true; }
        void BeginWorld(void* device)
        {
            EndWorld();
            if (!device) return;
            if (device != device_)
            {
                source.Release();
                device_ = device;
                ++generation_;
            }
            ++frame_;
            active_ = true;
            attempted_ = false;
            dispatched_ = false;
            earlyDispatched_ = false;
            transparentDispatched_ = false;
        }
        bool BeforeLiquids(int passType, uint32_t batchCount)
        {
            if (!active_ || !requested_ || attempted_ || dispatched_ || earlyDispatched_ || transparentDispatched_ || dispatching_ ||
                (passType != 0 && passType != 1) || batchCount == 0) return false;
            // Both native liquid buckets draw world water. Pass 1 can be the first/only
            // nonempty bucket; its number does not identify a secondary world view.
            // Do not retry later: a subsequent liquid pass can already contain water.
            attempted_ = true;
            WXL_SceneColor next{};
            next.structSize = sizeof(next);
            if (!source.Capture(device_, next) || !next.texture || !next.width || !next.height) return true;
            next.frameId = frame_;
            next.deviceGeneration = generation_;
            color_ = next;
            return true;
        }
        int GetSceneColor(WXL_SceneColor* out) const
        {
            if (!out || out->structSize != sizeof(*out)) return 0;
            *out = {};
            out->structSize = sizeof(*out);
            if (!active_ || !color_.texture) return 0;
            *out = color_;
            return 1;
        }
        bool CanDispatchTransparent(void* device) const { return CanDispatch(device) && !transparentDispatched_ && !earlyDispatched_; }
        void DispatchTransparent(void* device,bool effects=true) {
            if(!CanDispatchTransparent(device)) return;
            transparentDispatched_=true;
            if(!effects) return;
            dispatching_=true;
            const auto frame=frame_,generation=generation_;
            for(const auto& pass:passes_) {
                if(!active_ || frame_!=frame || generation_!=generation || device_!=device) break;
                if(pass.stage!=WXL_RENDER_BEFORE_TRANSPARENTS) continue;
                const WXL_RenderPassContext ctx{sizeof(ctx),pass.stage,frame,generation,device};
                pass.fn(pass.user,&ctx);
            }
            dispatching_=false;
        }
        bool CanDispatchEarly(void* device) const { return CanDispatch(device) && !earlyDispatched_; }
        void DispatchEarly(void* device,bool effects=true)
        {
            if(!CanDispatchEarly(device)) return;
            earlyDispatched_=true;
            if(!effects) return;
            dispatching_=true;
            const auto frame=frame_,generation=generation_;
            for(const auto& pass:passes_) {
                if(!active_ || frame_!=frame || generation_!=generation || device_!=device) break;
                if(pass.stage!=WXL_RENDER_BEFORE_SCREEN_EFFECTS) continue;
                const WXL_RenderPassContext ctx{sizeof(ctx),pass.stage,frame,generation,device};
                pass.fn(pass.user,&ctx);
            }
            dispatching_=false;
        }
        void Dispatch(void* device,bool effects=true)
        {
            if (!CanDispatch(device)) return;
            dispatched_ = true;
            if(!effects) return;
            dispatching_ = true;
            const auto frame = frame_, generation = generation_;
            for (const auto& pass : passes_)
            {
                if(pass.stage==WXL_RENDER_BEFORE_SCREEN_EFFECTS || pass.stage==WXL_RENDER_BEFORE_TRANSPARENTS) continue;
                // A callback can pump a reset/window event. Never continue with expired inputs.
                if (!active_ || frame_ != frame || generation_ != generation || device_ != device) break;
                const WXL_RenderPassContext ctx{sizeof(ctx), pass.stage, frame, generation, device};
                pass.fn(pass.user, &ctx);
            }
            dispatching_ = false;
        }
        void EndWorld()
        {
            active_ = false;
            color_ = {};
        }
        void DeviceLost()
        {
            EndWorld();
            source.Release();
            device_ = nullptr;
            ++generation_;
        }
    private:
        struct Pass { uint32_t stage; WXL_RenderPassFn fn; void* user; };
        static bool ValidStage(uint32_t stage)
        {
            return stage == WXL_RENDER_BEFORE_TRANSPARENTS || stage == WXL_RENDER_BEFORE_SCREEN_EFFECTS || stage == WXL_RENDER_ATMOSPHERE || stage == WXL_RENDER_WATER_VOLUME ||
                   stage == WXL_RENDER_GRADING || stage == WXL_RENDER_SHARPEN;
        }
        std::vector<Pass> passes_;
        WXL_SceneColor color_{};
        void* device_ = nullptr;
        uint64_t frame_ = 0, generation_ = 0;
        bool requested_ = false, active_ = false, attempted_ = false;
        bool dispatched_ = false, dispatching_ = false;
        bool earlyDispatched_ = false, transparentDispatched_ = false;
    };
}
