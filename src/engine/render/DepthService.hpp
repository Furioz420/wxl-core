// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "wxl/RenderDepthApi.h"
#include <d3d9.h>
namespace wxl::render::depth
{
    using GetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9**);
    using SetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*);
    const WXL_RenderDepthApi* Api();
    const WXL_MultisampleDepthApi* MultisampleApi();
    const WXL_WaterDepthApi* WaterApi();
    const WXL_LiquidDepthApi* LiquidApi();
    void CaptureWaterDepth(bool after=false);
    void BeginLiquidRead();
    void DiscardLiquidDepth();
    void Configure(GetFn get, SetFn set);
    void OnInput(void*, const void* args);
    void Begin(void* device, uint64_t frame, uint64_t generation);
    void BeforeClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, float z, bool mainWorld, void* caller=nullptr);
    bool AfterClear(IDirect3DDevice9* device, HRESULT result);
    HRESULT Set(IDirect3DDevice9* device, IDirect3DSurface9* surface);
    HRESULT Get(IDirect3DDevice9* device, IDirect3DSurface9** out);
    void SaveCamera();
    void BeginRead();
    void Preview();
    void EndRead();
    void End();
    void Reset();
}
