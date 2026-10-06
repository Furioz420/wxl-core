// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WXL_INTERIOR_API_H
#define WXL_INTERIOR_API_H
#include <stdint.h>
#define WXL_INTERIOR_API_VERSION 1
// Optional render-thread query, owned by the existing WMO viewer-locate hook.
// 0: unknown, 1: exterior/open-sided, 2: enclosed interior. Refreshed each scene
// update; a camera classification, not per-pixel visibility or light occlusion.
typedef struct WXL_InteriorApi {
    uint32_t structSize, apiVersion;
    int(__cdecl* ViewerEnvironment)(void);
} WXL_InteriorApi;
#ifdef __cplusplus
inline bool WxlViewerEnclosed(const WXL_InteriorApi* api)
{
    return api && api->structSize >= sizeof(*api) &&
        api->apiVersion == WXL_INTERIOR_API_VERSION && api->ViewerEnvironment &&
        api->ViewerEnvironment() == 2;
}
#endif
#endif
