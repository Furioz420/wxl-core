// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Fog argument callback; edits to caller-owned arguments reach the native setter.
#define WXL_FOG_ARGUMENT_BRIDGE(name,observe,original) \
__declspec(naked) void __cdecl name() { \
 __asm pushfd \
 __asm pushad \
 __asm mov eax,esp \
 __asm sub esp,528 \
 __asm and esp,-16 \
 __asm fxsave [esp] \
 __asm mov [esp+512],eax \
 __asm cld \
 __asm lea edx,[eax+40] \
 __asm push edx \
 __asm push dword ptr [eax+36] \
 __asm call observe \
 __asm add esp,8 \
 __asm fxrstor [esp] \
 __asm mov esp,[esp+512] \
 __asm popad \
 __asm popfd \
 __asm jmp dword ptr [original] \
}
// The pinned liquid-surface call has no stack arguments. Preserve native return state.
#define WXL_LIQUID_END_BRIDGE(name,observe,original) \
__declspec(naked) void __cdecl name() { \
 __asm call dword ptr [original] \
 __asm pushfd \
 __asm pushad \
 __asm mov eax,esp \
 __asm sub esp,528 \
 __asm and esp,-16 \
 __asm fxsave [esp] \
 __asm mov [esp+512],eax \
 __asm cld \
 __asm push dword ptr [eax+36] \
 __asm call observe \
 __asm add esp,4 \
 __asm fxrstor [esp] \
 __asm mov esp,[esp+512] \
 __asm popad \
 __asm popfd \
 __asm ret \
}
