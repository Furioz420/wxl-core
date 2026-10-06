// Reusable D3D scene-color copy. Copyright (C) 2026 WarcraftXL.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "SceneColorCopy.hpp"
namespace wxl::render
{
    class SceneColor
    {
    public:
        bool Capture(void* rawDevice, WXL_SceneColor& out);
        void Release() { copy_.Release(); }
    private:
        SceneColorCopy<IDirect3DDevice9, IDirect3DTexture9, IDirect3DSurface9> copy_;
        unsigned failureReports_ = 0;
    };
}