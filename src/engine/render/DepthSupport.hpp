// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d9.h>
#include <wrl/client.h>
namespace wxl::render
{
    inline bool SupportsIntz(IDirect3DDevice9* device, D3DFORMAT color)
    {
        constexpr auto format = static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z'));
        Microsoft::WRL::ComPtr<IDirect3D9> api;
        D3DDEVICE_CREATION_PARAMETERS params{};
        D3DDISPLAYMODE mode{};
        return SUCCEEDED(device->GetDirect3D(api.GetAddressOf())) && api &&
            SUCCEEDED(device->GetCreationParameters(&params)) &&
            SUCCEEDED(api->GetAdapterDisplayMode(params.AdapterOrdinal, &mode)) &&
            SUCCEEDED(api->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format,
                D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, format)) &&
            SUCCEEDED(api->CheckDepthStencilMatch(params.AdapterOrdinal, params.DeviceType,
                mode.Format, color, format));
    }
}
