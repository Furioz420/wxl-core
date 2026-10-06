// Shared color-copy implementation. Copyright (C) 2026 WarcraftXL.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "wxl/RenderApi.h"
#include <d3d9.h>
#include <wrl/client.h>

namespace wxl::render
{
    // The production instantiation uses D3D9 COM types. Tests supply the same small interface
    // surface to exercise allocation, resolve failures, resize and ownership without a GPU.
    template<class Device, class Texture, class Surface> class SceneColorCopy
    {
    public:
        void Release()
        {
            surface_.Reset();
            texture_.Reset();
            owner_ = nullptr;
            width_ = height_ = 0;
            format_ = D3DFMT_UNKNOWN;
        }

        bool Capture(void* rawDevice, WXL_SceneColor& out)
        {
            error_ = D3DERR_INVALIDCALL;
            auto* device = static_cast<Device*>(rawDevice);
            if (!device) return false;
            Microsoft::WRL::ComPtr<Surface> source;
            D3DSURFACE_DESC desc{};
            D3DVIEWPORT9 viewport{};
            if (FAILED(error_ = device->GetRenderTarget(0, source.GetAddressOf())) || !source ||
                FAILED(error_ = source->GetDesc(&desc)) ||
                FAILED(error_ = device->GetViewport(&viewport))) return false;
            // UVs describe the whole world target. A subviewport needs an explicit UV contract.
            error_ = D3DERR_INVALIDCALL;
            if (!desc.Width || !desc.Height || viewport.X || viewport.Y ||
                viewport.Width != desc.Width || viewport.Height != desc.Height) return false;

            if (owner_ != device || width_ != desc.Width || height_ != desc.Height ||
                format_ != desc.Format || !texture_ || !surface_)
            {
                Release();
                Microsoft::WRL::ComPtr<Texture> texture;
                Microsoft::WRL::ComPtr<Surface> surface;
                if (FAILED(error_ = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
                        desc.Format, D3DPOOL_DEFAULT, texture.GetAddressOf(), nullptr)) || !texture ||
                    FAILED(error_ = texture->GetSurfaceLevel(0, surface.GetAddressOf())) || !surface) return false;
                texture_ = texture;
                surface_ = surface;
                owner_ = device;
                width_ = desc.Width;
                height_ = desc.Height;
                format_ = desc.Format;
            }

            // Resolve the ACTIVE target (possibly MSAA), never a previous-frame backbuffer.
            // Same dimensions/format and NONE filtering need no shader/sampler/state changes.
            error_ = device->StretchRect(source.Get(), nullptr, surface_.Get(), nullptr, D3DTEXF_NONE);
            if (FAILED(error_)) return false;
            out.width = width_;
            out.height = height_;
            out.format = static_cast<uint32_t>(format_);
            out.texture = texture_.Get();
            return true;
        }
        HRESULT Error() const { return error_; }

    private:
        Microsoft::WRL::ComPtr<Texture> texture_;
        Microsoft::WRL::ComPtr<Surface> surface_;
        Device* owner_ = nullptr;
        UINT width_ = 0, height_ = 0;
        D3DFORMAT format_ = D3DFMT_UNKNOWN;
        HRESULT error_ = S_OK;
    };
}