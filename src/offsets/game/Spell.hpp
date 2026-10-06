// Spell-text lookup landmarks for build 12340.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <cstdint>

namespace wxl::offsets::game::spell
{
    constexpr uintptr_t kSpellStorage = 0x00AD49D0;
    constexpr uintptr_t kGetLocalizedRow = 0x004CFD20;
    constexpr uintptr_t kParseSpellText = 0x0057ABC0;

    using GetLocalizedRowFn = int(__thiscall*)(void* storage, uint32_t id, void* output);
    using ParseSpellTextFn = void(__cdecl*)(void* record, void* destination, uint32_t capacity,
                                            uint32_t, uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t);
}
