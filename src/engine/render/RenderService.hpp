// Core-owned render service lifecycle. Copyright (C) 2026 WarcraftXL.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "wxl/RenderApi.h"
#include "wxl/RenderControlApi.h"
namespace wxl::render
{
    const WXL_RenderApi* Api();
    const WXL_RenderControlApi* ControlApi();
    void OnComparisonInput(void*,const void*);
    void BeginWorld(void* device);
    void NativeWorldBegin(void* device);
    void BeforeLiquids(int passType, uint32_t batchCount, bool mainWorld);
    void Dispatch(void* device);
    void BeforeScreenEffects(void* device);
    void BeforeTransparents(void* device);
    void EndWorld();
    void DeviceLost();
}
