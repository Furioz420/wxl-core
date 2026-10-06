// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
namespace wxl::render {
// Each participating module observes the same existing input broadcast.
// No new window hook; auto-repeat and focus loss cannot trigger reloads.
class ReloadKey {
    bool down_=false;
public:
    bool Handle(UINT message,WPARAM key,bool* handled) {
        if(message==WM_KILLFOCUS || (message==WM_ACTIVATEAPP && !key)) down_=false;
        if(key!=VK_F12) return false;
        if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN) {
            down_=true; if(handled) *handled=true; return false;
        }
        if(message==WM_KEYUP || message==WM_SYSKEYUP) {
            const bool fire=down_; down_=false;
            if(handled) *handled=true;
            return fire;
        }
        return false;
    }
};
}
