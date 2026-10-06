// Curated spell-text binding for addon-facing compatibility helpers.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include "game/Binding.hpp"
#include "offsets/game/Spell.hpp"

#include <windows.h>

#include <cstdint>

namespace wxl::game::spell
{
    inline bool Description(uint32_t spellId, char* destination, uint32_t capacity)
    {
        if (!spellId || !destination || capacity == 0) return false;
        destination[0] = '\0';

        uint8_t record[680]{};
        bool found = false;
        __try
        {
            found = Native<offsets::game::spell::GetLocalizedRowFn>(
                offsets::game::spell::kGetLocalizedRow)(
                    reinterpret_cast<void*>(offsets::game::spell::kSpellStorage),
                    spellId, record) != 0;
            if (found)
                Native<offsets::game::spell::ParseSpellTextFn>(
                    offsets::game::spell::kParseSpellText)(
                        record, destination, capacity, 0, 0, 0, 0, 1, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            destination[0] = '\0';
            found = false;
        }
        return found && destination[0] != '\0';
    }
}
