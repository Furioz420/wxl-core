// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#include "SceneColor.hpp"
#include "RenderPerformance.hpp"
#include "common/Log.hpp"
namespace wxl::render
{
    bool SceneColor::Capture(void* rawDevice, WXL_SceneColor& out)
    {
        perf::Scope timing(perf::Stage::Copy);
        ++perf::copies;
        if (copy_.Capture(rawDevice, out)) return true;
        if (failureReports_++ < 4)
            WLOG_WARN("render-frame: scene copy unavailable hr=0x%08lX; use normal water fallback",
                      static_cast<unsigned long>(copy_.Error()));
        return false;
    }
}
