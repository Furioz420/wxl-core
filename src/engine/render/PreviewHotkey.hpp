// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>

namespace wxl::render::depth
{
    // Use the existing window input stream: no global key polling while another app has focus.
    class PreviewHotkey
    {
        bool pressed_ = false;
    public:
        bool Handle(UINT message, WPARAM key, bool& preview, WPARAM binding = VK_F8)
        {
            if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !key))
                pressed_ = false;
            if (key != binding) return false;
            if (message == WM_KEYDOWN) { pressed_ = true; return true; }
            if (message != WM_KEYUP) return false;
            if (pressed_) preview = !preview;
            pressed_ = false;
            return true;
        }
    };
}
