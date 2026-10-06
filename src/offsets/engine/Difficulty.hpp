// Dungeon-difficulty landmarks for the 3.3.5a (12340) client.
// Copyright (C) 2026 WarcraftXL

#pragma once

#include <cstdint>

namespace wxl::offsets::engine::difficulty
{
    // Script_SetDungeonDifficulty converts the Lua value from 1-based to the
    // packet's zero-based value, then compares it with 1. Raising this immediate
    // to 2 enables Blizzard's dormant DUNGEON_DIFFICULTY3 / Epic slot.
    constexpr uintptr_t kSetDungeonDifficultyMaximum = 0x005260A9;
    constexpr uint8_t kStockMaximum = 1;
    constexpr uint8_t kMythicMaximum = 2;

    // CGGameObject_C_Type_DungeonDifficulty::UpdateDisabled. The native
    // function is a thiscall with one callee-popped stack argument (ret 4).
    // Type-31 game objects use property 0x58 for their zero-based difficulty.
    constexpr uintptr_t kDungeonDifficultyUpdateDisabled = 0x00710A50;
    // CGGameObject_C::GetModelFileNameInternal. This is the final
    // GameObjectDisplayInfo path lookup used while a game object's M2 is
    // created, and therefore the narrow point where the two stock dungeon
    // layers can receive WarcraftXL-owned Normal/Heroic/Mythic portal assets.
    constexpr uintptr_t kGameObjectGetModelFileNameInternal = 0x0070EE30;
    // CGObject_C::SetModel swaps one live object's CM2Model and updates the
    // already-created World object. It is used only when the reused Heroic
    // dungeon layer changes role between Heroic and Mythic in place.
    constexpr uintptr_t kGameObjectSetModel = 0x00743730;
    constexpr uintptr_t kGetGameObjectPropertyIndex = 0x00746190;
    constexpr uintptr_t kGetCurrentDungeonDifficulty = 0x005138D0;

    constexpr uint32_t kDungeonDifficultyGameObjectType = 31;
    constexpr uint32_t kGameObjectOwnerOffset = 0x04;
    constexpr uint32_t kObjectDescriptorsOffset = 0x08;
    constexpr uint32_t kObjectEntryDescriptorOffset = 0x0C;
    constexpr uint32_t kGameObjectRenderStateOffset = 0xB8;
    constexpr uint32_t kGameObjectModelOffset = 0xB4;
    constexpr uint32_t kGameObjectDefinitionOffset = 0xD0;
    constexpr uint32_t kGameObjectStatsOffset = 0x1A4;
    constexpr uint32_t kGameObjectTypeOffset = 0x2D;
    constexpr uint32_t kGameObjectPropertyValuesOffset = 0x14;
    constexpr uint32_t kRenderStateFlagsOffset = 0x7C;
    constexpr uint32_t kGameObjectPropertyCount = 24;
    constexpr uint32_t kDifficultyProperty = 0x58;
    constexpr uint32_t kDisabledRenderFlag = 0x04;
    constexpr uint32_t kMapInstanceTypeOffset = 0x08;
    constexpr uint32_t kRaidInstanceType = 2;

    using DungeonDifficultyUpdateDisabledFn =
        void(__fastcall*)(void* typeOwner, void* edx, uint32_t updateArg);
    using GameObjectGetModelFileNameInternalFn =
        const char*(__fastcall*)(void* gameObject, void* edx);
    using GameObjectSetModelFn =
        void(__fastcall*)(void* gameObject, void* edx, void* model);
    using GetGameObjectPropertyIndexFn =
        int32_t(__cdecl*)(int32_t gameObjectType, int32_t property);
    using GetCurrentDungeonDifficultyFn = uint32_t(__cdecl*)();
}
