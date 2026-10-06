// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WXL_RENDER_CONTROL_API_H
#define WXL_RENDER_CONTROL_API_H
#include <stdint.h>
#define WXL_RENDER_CONTROL_API_VERSION 1
// Optional, render-thread-only service: wxl.render-control. State is latched at
// world begin, so a comparison never splits one frame between two modes.
typedef struct WXL_RenderControlApi {
    uint32_t structSize,apiVersion;
    int(__cdecl* Enabled)(void);
} WXL_RenderControlApi;
#ifdef __cplusplus
inline bool WxlRenderEffectsEnabled(const WXL_RenderControlApi* api)
{
    // Older cores keep existing behavior. Do not call through an incompatible ABI.
    return !api || api->structSize<sizeof(*api) || api->apiVersion!=WXL_RENDER_CONTROL_API_VERSION || !api->Enabled || api->Enabled()!=0;
}
#endif
#endif
