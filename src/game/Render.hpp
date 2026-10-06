// Optional shared renderer client. Copyright (C) 2026 WarcraftXL.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/Extensions.hpp"
#include "wxl/RenderApi.h"
#include "wxl/RenderControlApi.h"
namespace wxl::game::render
{
    inline bool EffectsEnabled()
    {
        static const auto* api=static_cast<const WXL_RenderControlApi*>(
            wxl::runtime::extensions::GetInterface("wxl.render-control",WXL_RENDER_CONTROL_API_VERSION));
        return WxlRenderEffectsEnabled(api);
    }
    inline const WXL_RenderApi* Api()
    {
        auto* api = static_cast<const WXL_RenderApi*>(
            wxl::runtime::extensions::GetInterface("wxl.render", WXL_RENDER_API_VERSION));
        return api && api->structSize >= sizeof(*api) && api->apiVersion == WXL_RENDER_API_VERSION &&
            api->passContextSize == sizeof(WXL_RenderPassContext) && api->sceneColorSize == sizeof(WXL_SceneColor) &&
            api->AddPostProcess && api->RequestSceneColor && api->GetSceneColor ? api : nullptr;
    }
}
