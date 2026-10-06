// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace wxl::render {
using WorldScopeCallback=void(*)(void*);
inline void RunNativeWorld(WorldScopeCallback begin,WorldScopeCallback draw,WorldScopeCallback end,void* context) {
    __try { begin(context); draw(context); }
    __finally { end(context); }
}
}
