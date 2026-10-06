// Ground-effect game bindings: typed access to the live grass shader constant block.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#pragma once

#include "offsets/game/GroundEffect.hpp"

/**
 * @brief Typed accessors for the client ground-effect renderer.
 *
 * Extensions attach to the corresponding functions through the stable named
 * hook points. This facade exposes only the runtime data contract they need;
 * private addresses remain owned by the core offset catalog.
 */
namespace wxl::game::groundeffect
{
    namespace off = wxl::offsets::game::groundeffect;

    /// Signature of the `Grass.InitShaderConstants` named hook point.
    using InitShaderConstantsFn = void(__cdecl*)();

    /**
     * @brief Returns the first float of the free c14..c22 grass constant range.
     * @return Pointer to float4[kFreeVertexConstantCount], valid on the render thread.
     */
    inline float* FreeVertexConstants()
    {
        return reinterpret_cast<float*>(off::kVsConstantBlock) +
               off::kVsFirstFreeReg * 4;
    }

    /// Number of float4 registers available from FreeVertexConstants().
    inline constexpr unsigned kFreeVertexConstantCount = off::kVsFreeRegCount;
}
