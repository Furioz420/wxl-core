// Companion-journal sorting landmarks for build 12340.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <cstdint>

namespace wxl::offsets::game::companion
{
    constexpr uintptr_t kSort = 0x0053C7C0;
    using SortFn = int(__cdecl*)(const uint32_t* left, const uint32_t* right);
}
