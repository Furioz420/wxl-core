// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#define WXL_AUTHORED_GRADING_API_VERSION 1
// Render thread only. Copies values, never GPU resources. Exact frame/generation required.
struct WXL_AuthoredGrading {
    uint32_t structSize;
    float strength;
    float curve[32];
};
struct WXL_AuthoredGradingApi {
    uint32_t structSize;
    uint32_t apiVersion;
    int (__cdecl* GetCurve)(uint64_t frame, uint64_t generation, WXL_AuthoredGrading* out);
};
