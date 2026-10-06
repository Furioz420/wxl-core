// Shared world-frame inputs and ordered post-processing. Copyright (C) 2026 WarcraftXL.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WXL_RENDER_API_H
#define WXL_RENDER_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define WXL_RENDER_API_VERSION 1

// Run before legacy OnWorldRenderEnd subscribers (outlines, markers) and native UI.
// Same-stage callbacks retain registration order. Stages do not depend on DLL load order.
enum WXL_RenderStage
{
    // Optional early boundary; never replayed by late Dispatch. Register a separate
    // late fallback when needed. Existing stage values and ABI stay unchanged.
    WXL_RENDER_BEFORE_TRANSPARENTS = 40,
    WXL_RENDER_BEFORE_SCREEN_EFFECTS = 50,
    WXL_RENDER_ATMOSPHERE = 100,
    WXL_RENDER_WATER_VOLUME = 200,
    WXL_RENDER_GRADING = 300,
    WXL_RENDER_SHARPEN = 400
};

typedef struct WXL_RenderPassContext
{
    uint32_t structSize;
    uint32_t stage;
    uint64_t frameId;          // World-render invocation, not Present count.
    uint64_t deviceGeneration; // Changes on device replacement or any reset attempt.
    void* device;             // Borrowed IDirect3DDevice9*, only during this callback.
} WXL_RenderPassContext;

typedef struct WXL_SceneColor
{
    uint32_t structSize;       // Caller sets sizeof(WXL_SceneColor).
    uint32_t width;
    uint32_t height;
    uint32_t format;           // D3DFORMAT of texture; matches the captured color target.
    uint64_t frameId;
    uint64_t deviceGeneration;
    void* texture;             // Borrowed IDirect3DTexture9*, before the first nonempty world liquid bucket.
} WXL_SceneColor;

typedef void(__cdecl* WXL_RenderPassFn)(void* user, const WXL_RenderPassContext* context);

// GetInterface("wxl.render", WXL_RENDER_API_VERSION). All calls use the client/render thread.
// Register at module load. Callbacks are process-lifetime, must not throw, and must restore
// all GPU state they touch. This service does not own the M2 DrawIndexedPrimitive hook.
typedef struct WXL_RenderApi
{
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t passContextSize;
    uint32_t sceneColorSize;
    // Returns 0 for an unknown stage, null callback, duplicate (fn,user), or during dispatch.
    int(__cdecl* AddPostProcess)(uint32_t stage, WXL_RenderPassFn fn, void* user);
    // Process-lifetime request; no allocation/copy until the first nonempty world liquid bucket (0 or 1).
    void(__cdecl* RequestSceneColor)(void);
    // Returns 1 only after successful capture in the CURRENT world render. A correctly sized
    // output is cleared on failure; wrong-size/null output is untouched.
    // The borrowed texture expires at world end, next world begin, Present, or device loss/reset.
    // Do not Release/AddRef/cache/bind beyond that lifetime. Post-process passes must capture
    // their own fresh color: this texture always describes PRE-WATER color.
    int(__cdecl* GetSceneColor)(WXL_SceneColor* out);
} WXL_RenderApi;
#ifdef __cplusplus
}
#endif
#endif
