// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/Log.hpp"
#include <chrono>

namespace wxl::render::perf
{
    // CPU submission durations only. No query readback, GPU wait or per-frame log flushing.
    using Clock = std::chrono::steady_clock;
    enum class Stage { Copy, DepthRead, PostProcess, Preview, WaterDepthCopy };
    inline double milliseconds[5]{};
    inline unsigned frames = 0, copies = 0;
    inline unsigned liquidCalls[2]{}, nonemptyLiquids[2]{}, outsideWorld = 0;
    inline double Ms(Clock::duration time)
    { return std::chrono::duration<double, std::milli>(time).count(); }
    class Scope
    {
        Stage stage_;
        Clock::time_point start_ = Clock::now();
    public:
        explicit Scope(Stage stage) : stage_(stage) {}
        ~Scope() { milliseconds[static_cast<unsigned>(stage_)] += Ms(Clock::now() - start_); }
    };
    inline void Frame()
    {
        const auto now = Clock::now();
        static auto window = now;
        ++frames;
        if (Ms(now - window) < 10000.0) return;
        WLOG_INFO("render-perf-v1: frames=%u copies=%u cpu_per_frame_ms[scene-copy/depth-read/post-process/preview]=%.3f/%.3f/%.3f/%.3f",
            frames, copies, milliseconds[0]/frames, milliseconds[1]/frames,
            milliseconds[2]/frames, milliseconds[3]/frames);
        WLOG_INFO("render-liquid-v1: calls[0/1]=%u/%u nonempty[0/1]=%u/%u outside-main-world=%u",
            liquidCalls[0], liquidCalls[1], nonemptyLiquids[0], nonemptyLiquids[1], outsideWorld);
        WLOG_INFO("render-water-depth-perf: cpu_per_frame_ms=%.3f",milliseconds[4]/frames);
        for (double& value : milliseconds) value = 0;
        frames = copies = 0;
        liquidCalls[0] = liquidCalls[1] = nonemptyLiquids[0] = nonemptyLiquids[1] = outsideWorld = 0;
        window = now;
    }
}
