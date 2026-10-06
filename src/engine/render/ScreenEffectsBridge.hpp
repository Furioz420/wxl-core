// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// x86 bridge shared with the ABI regression test. Pushad's original ESP follows
// pushfd, so the native return address is 36 bytes above the saved register block.
#define WXL_SCREEN_EFFECTS_BRIDGE(name,before,original) \
__declspec(naked) void __cdecl name() { \
    __asm pushfd \
    __asm pushad \
    __asm mov eax, esp \
    __asm sub esp, 528 \
    __asm and esp, -16 \
    __asm fxsave [esp] \
    __asm mov [esp+512], eax \
    __asm cld \
    __asm push dword ptr [eax+36] \
    __asm call before \
    __asm add esp, 4 \
    __asm fxrstor [esp] \
    __asm mov esp, [esp+512] \
    __asm popad \
    __asm popfd \
    __asm jmp dword ptr [original] \
}
