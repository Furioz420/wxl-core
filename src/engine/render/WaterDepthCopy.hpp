// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d9.h>
#include <wrl/client.h>
namespace wxl::render
{
inline constexpr char kWaterDepthCopyShader[] =
    "sampler2D depth:register(s0); float4 main(float2 uv:TEXCOORD0):COLOR0"
    "{return tex2D(depth,uv).rrrr;}";

// Caller temporarily detaches the source depth surface. This pass owns only a
// reusable colour texture, so sampling the result never aliases live depth.
class WaterDepthCopy
{
    Microsoft::WRL::ComPtr<IDirect3DTexture9> texture_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> surface_;
    UINT width_=0, height_=0;
public:
    void Reset() { surface_.Reset(); texture_.Reset(); width_=height_=0; }
    IDirect3DTexture9* Texture() const { return texture_.Get(); }
    bool Copy(IDirect3DDevice9* d, IDirect3DTexture9* source,
              IDirect3DPixelShader9* shader, UINT width, UINT height)
    {
        if (!d || !source || !shader || !width || !height) return false;
        D3DVIEWPORT9 vp{};
        Microsoft::WRL::ComPtr<IDirect3DSurface9> target;
        Microsoft::WRL::ComPtr<IDirect3DStateBlock9> state;
        if (FAILED(d->GetViewport(&vp)) || vp.X || vp.Y || vp.Width!=width || vp.Height!=height ||
            FAILED(d->GetRenderTarget(0,target.GetAddressOf()))) return false;
        for (DWORD i=1;i<4;++i)
        {
            Microsoft::WRL::ComPtr<IDirect3DSurface9> extra;
            if (SUCCEEDED(d->GetRenderTarget(i,extra.GetAddressOf())) && extra) return false;
        }
        if (width_!=width || height_!=height) Reset();
        if (!texture_)
        {
            if (FAILED(d->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,
                D3DPOOL_DEFAULT,texture_.GetAddressOf(),nullptr)) ||
                FAILED(texture_->GetSurfaceLevel(0,surface_.GetAddressOf()))) { Reset(); return false; }
            width_=width; height_=height;
        }
        if (FAILED(d->CreateStateBlock(D3DSBT_ALL,state.GetAddressOf())) || FAILED(state->Capture())) return false;
        bool ok=true;
        const auto set=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
        set(d->SetRenderTarget(0,surface_.Get()));
        set(d->SetViewport(&vp));
        set(d->SetVertexShader(nullptr)); set(d->SetPixelShader(shader));
        set(d->SetRenderState(D3DRS_ZENABLE,FALSE)); set(d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE));
        set(d->SetRenderState(D3DRS_STENCILENABLE,FALSE)); set(d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE));
        set(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE)); set(d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE));
        set(d->SetRenderState(D3DRS_FOGENABLE,FALSE)); set(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
        set(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID)); set(d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE));
        set(d->SetRenderState(D3DRS_COLORWRITEENABLE,15)); set(d->SetRenderState(D3DRS_CLIPPLANEENABLE,0));
        set(d->SetTexture(0,source));
        set(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
        set(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
        set(d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
        set(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
        set(d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
        set(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE));
        struct V {float x,y,z,w,u,v;};
        const float x=static_cast<float>(width)-.5f,y=static_cast<float>(height)-.5f;
        const V quad[]{{-.5f,-.5f,0,1,0,0},{x,-.5f,0,1,1,0},{-.5f,y,0,1,0,1},{x,y,0,1,1,1}};
        set(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
        if (ok) set(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(V)));
        // RTs are not in a D3D9 state block. Restore them before viewport/state;
        // Apply also removes the sampled INTZ before the caller rebinds it.
        set(d->SetRenderTarget(0,target.Get()));
        set(state->Apply()); set(d->SetViewport(&vp));
        return ok;
    }
};
}
