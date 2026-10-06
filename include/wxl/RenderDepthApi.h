// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WXL_RENDER_DEPTH_API_H
#define WXL_RENDER_DEPTH_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define WXL_RENDER_DEPTH_API_VERSION 2
typedef struct WXL_SceneDepth
{
    uint32_t structSize, width, height, format;
    uint64_t frameId, deviceGeneration;
    void* texture; // Borrowed IDirect3DTexture9; never retain or release.
    float view[16], projection[16]; // Row-major, D3D row-vector, native world camera.
    float cameraPosition[3];
    float depthMinZ, depthMaxZ; // Viewport mapping: sampledZ = MinZ + ndcZ * (MaxZ-MinZ).
} WXL_SceneDepth;
// GetInterface("wxl.render-depth", 2). Client/render thread only. Version 1 is retired:
// it lacked the viewport depth interval required by the native world's MaxZ=0.94.
// Opt-in experimental capability. Request before a world frame. Unsupported targets/MSAA
// remain native. GetSceneDepth succeeds ONLY during ordered wxl.render callbacks, when
// the service has temporarily unbound its depth surface. Restore all touched state,
// including texture bindings, before returning. No water-surface/coverage guarantee.
typedef struct WXL_RenderDepthApi
{
    uint32_t structSize, apiVersion, sceneDepthSize;
    void(__cdecl* RequestSceneDepth)(void);
    // Correctly sized outputs are cleared on failure; null/wrong-size are untouched.
    int(__cdecl* GetSceneDepth)(WXL_SceneDepth* out);
} WXL_RenderDepthApi;
// Independent pre-liquid R32F copy of raw device depth. Safe to sample while native
// INTZ remains bound for water depth testing/writing. Same frame/generation and
// dimensions as wxl.render scene colour required. Borrowed until EndWorld/Reset.
// RequestCopy does not enable experimental depth capture: WXL_RENDER_DEPTH must
// already be enabled. Missing/unsupported depth returns unavailable, never stale.
#define WXL_WATER_DEPTH_API_VERSION 1
typedef struct WXL_WaterDepthApi
{
    uint32_t structSize, apiVersion, sceneDepthSize;
    void(__cdecl* RequestCopy)(void);
    int(__cdecl* GetDepth)(WXL_SceneDepth* out);
} WXL_WaterDepthApi;
// Independent liquid-boundary copy. Request before the frame; GetDepth is valid in
// OnLiquidRenderBegin/End only and expires when that callback returns.
// The original wxl.water-depth pre-liquid snapshot is never overwritten.
#define WXL_LIQUID_DEPTH_API_VERSION 1
typedef WXL_WaterDepthApi WXL_LiquidDepthApi;
// MSAA depth remains bound. Available inside ordered render callbacks and
// OnLiquidRenderBegin/End. The core may supply matching D24S8 storage when the
// native D24X8 surface has no stencil; native callers retain their own identity.
// Surface is borrowed until callback return: no retention or depth-value clears.
// Restore all bindings; late per-sample composites may use stencil as scratch.
// Consumers resolve into their own texture; existing single-sample ABI is unchanged.
#define WXL_MULTISAMPLE_DEPTH_API_VERSION 1
typedef struct WXL_MultisampleDepth {
    uint32_t structSize;
    WXL_SceneDepth frame; // texture is null; capture format and camera metadata only.
    void* surface;
} WXL_MultisampleDepth;
typedef struct WXL_MultisampleDepthApi {
    uint32_t structSize,apiVersion;
    void(__cdecl* Request)(void);
    int(__cdecl* GetDepth)(WXL_MultisampleDepth* out);
} WXL_MultisampleDepthApi;
#ifdef __cplusplus
}
#endif
#endif
