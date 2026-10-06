// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
namespace wxl::render::defaults {
// Accepted integration is available without a test launcher. Explicit zero opts out;
// malformed overrides fail closed. Depth value 2 is the diagnostic preview only.
inline unsigned Parse(const char* value,unsigned size,bool depth=false) {
    if(size==0) return 1;
    if(size!=1 || !value) return 0;
    if(value[0]=='1') return 1;
    return depth && value[0]=='2' ? 2 : 0;
}
inline unsigned Read(const char* name,bool depth=false) {
    char value[8]{}; auto size=GetEnvironmentVariableA(name,value,sizeof(value));
    return Parse(value,size,depth);
}
inline bool Enabled(const char* name) { return Read(name)!=0; }
}
