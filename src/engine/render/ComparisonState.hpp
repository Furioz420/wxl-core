// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
namespace wxl::render {
class ComparisonState {
    bool requested_=true,enabled_=true,pressed_=false;
public:
    bool Enabled() const { return enabled_; }
    bool BeginWorld() { bool changed=enabled_!=requested_; enabled_=requested_; return changed; }
    bool Input(UINT message,WPARAM key,LPARAM flags) {
        if(message==WM_KILLFOCUS || (message==WM_ACTIVATEAPP && !key)) pressed_=false;
        if(key!=VK_F11) return false;
        if(message==WM_KEYDOWN) { if(!(flags&(1u<<30))) pressed_=true; return true; }
        if(message!=WM_KEYUP) return false;
        if(pressed_) requested_=!requested_;
        pressed_=false; return true;
    }
};
}
