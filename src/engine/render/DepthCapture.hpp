// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <cmath>

namespace wxl::render
{
    enum class DepthStatus { Waiting, Ready, Multisampled, Format, Viewport, Target,
        Allocation, Bind, ClearFailed, ClearedAgain, ReadFailed, RestoreFailed, Camera, Capability, Stencil };

    // D3D calls are injected only to bypass our Set/Get vtable slots and to test real ownership
    // and rollback behavior without a GPU. All other calls use the actual device interface.
    template<class Device, class Texture, class Surface> class DepthCapture
    {
        template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    public:
        using GetFn = HRESULT(__stdcall*)(Device*, Surface**);
        using SetFn = HRESULT(__stdcall*)(Device*, Surface*);
        GetFn get = nullptr;
        SetFn set = nullptr;
        bool allowMultisampled = false;
        bool (*supports)(Device*, D3DFORMAT) = nullptr;
        static constexpr D3DFORMAT Format = static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z'));

        bool Begin(Device* device)
        {
            if (!End()) return false;
            if (device != owner_) { ReleaseTexture(); owner_ = device; }
            active_ = device && get && set;
            attempted_ = valid_ = pending_ = false;
            status_ = DepthStatus::Waiting;
            viewport_ = {}; colorDesc_ = {}; depthDesc_ = {};
            return active_;
        }
        void BeforeClear(DWORD count, const D3DRECT* rects, DWORD flags, float z, bool mainWorld)
        {
            if (!active_ || !(flags & D3DCLEAR_ZBUFFER)) return;
            if (redirected_ || multisampled_)
            {
                // Driver protocols (including NVAPI depth resolve) can issue a Clear
                // rectangle entirely outside the surface. Forward it unchanged, but
                // do not invalidate world depth when no depth texel can be touched.
                if(count && rects && count<=256 && width_ && height_) {
                    bool intersects=false;
                    for(DWORD i=0;i<count;++i) {
                        const auto& r=rects[i];
                        if(r.x2>r.x1 && r.y2>r.y1 && r.x2>0 && r.y2>0 &&
                            static_cast<long long>(r.x1)<width_ && static_cast<long long>(r.y1)<height_) {
                            intersects=true; break;
                        }
                    }
                    if(!intersects) return;
                }
                Ptr<Surface> current;
                if (FAILED(get(owner_, current.GetAddressOf())) || current.Get() == (redirected_ ? surface_.Get() : native_.Get()))
                { valid_ = false; status_ = DepthStatus::ClearedAgain; }
                return;
            }
            if (attempted_ || !mainWorld || count || rects || z != 1.0f) return;
            Ptr<Surface> color, back, native;
            D3DSURFACE_DESC c{}, d{};
            D3DVIEWPORT9 vp{};
            if (FAILED(owner_->GetRenderTarget(0, color.GetAddressOf())) || !color ||
                FAILED(owner_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.GetAddressOf())) ||
                color.Get() != back.Get()) { status_ = DepthStatus::Target; return; }
            if (FAILED(get(owner_, native.GetAddressOf())) || !native ||
                FAILED(color->GetDesc(&c)) || FAILED(native->GetDesc(&d)) ||
                FAILED(owner_->GetViewport(&vp))) { status_ = DepthStatus::Target; return; }
            attempted_ = true;
            viewport_ = vp; colorDesc_ = c; depthDesc_ = d;
            const bool msaa=c.MultiSampleType!=D3DMULTISAMPLE_NONE || d.MultiSampleType!=D3DMULTISAMPLE_NONE;
            if(msaa && (!allowMultisampled || c.MultiSampleType!=d.MultiSampleType || c.MultiSampleQuality!=d.MultiSampleQuality))
            { status_=DepthStatus::Multisampled; return; }
            if ((d.Format != D3DFMT_D24S8 && d.Format != D3DFMT_D24X8) ||
                c.Width != d.Width || c.Height != d.Height || !c.Width || !c.Height)
            { status_ = DepthStatus::Format; return; }
            if (vp.X || vp.Y || vp.Width != c.Width || vp.Height != c.Height ||
                !std::isfinite(vp.MinZ) || !std::isfinite(vp.MaxZ) ||
                vp.MinZ < 0.0f || vp.MaxZ > 1.0f || vp.MaxZ <= vp.MinZ)
            { status_ = DepthStatus::Viewport; return; }
            if (d.Format == D3DFMT_D24S8 && !(flags & D3DCLEAR_STENCIL))
            { status_ = DepthStatus::Stencil; return; }

            if(msaa) {
                if(d.Format==D3DFMT_D24X8) {
                    // The native surface has no stencil. Allocate matching sample storage before
                    // the first world clear; all geometry then writes the same depth precision.
                    D3DSURFACE_DESC previous{};
                    if(texture_ || !surface_ || FAILED(surface_->GetDesc(&previous)) ||
                        previous.Width!=d.Width || previous.Height!=d.Height ||
                        previous.MultiSampleType!=d.MultiSampleType || previous.MultiSampleQuality!=d.MultiSampleQuality) {
                        ReleaseTexture();
                        if(FAILED(owner_->CreateDepthStencilSurface(d.Width,d.Height,D3DFMT_D24S8,
                            d.MultiSampleType,d.MultiSampleQuality,TRUE,surface_.GetAddressOf(),nullptr))) {
                            status_=DepthStatus::Allocation; return;
                        }
                    }
                    if(FAILED(set(owner_,surface_.Get()))) { status_=DepthStatus::Bind; return; }
                    redirected_=true;
                } else { ReleaseTexture(); }
                width_=c.Width; height_=c.Height;
                native_=native; multisampled_=pending_=true;
                return; // The original world clear initializes depth; effects initialize their stencil scratch.
            }
            if (!texture_ || width_ != c.Width || height_ != c.Height || colorFormat_ != c.Format)
            {
                ReleaseTexture();
                if (!supports || !supports(owner_, c.Format)) { status_ = DepthStatus::Capability; return; }
                Ptr<Texture> texture;
                Ptr<Surface> surface;
                if (FAILED(owner_->CreateTexture(c.Width, c.Height, 1, D3DUSAGE_DEPTHSTENCIL,
                        Format, D3DPOOL_DEFAULT, texture.GetAddressOf(), nullptr)) || !texture ||
                    FAILED(texture->GetSurfaceLevel(0, surface.GetAddressOf())) || !surface)
                { status_ = DepthStatus::Allocation; return; }
                texture_ = texture; surface_ = surface;
                width_ = c.Width; height_ = c.Height;
                colorFormat_ = c.Format;
            }
            // Commit the redirect only after a successful bind. The caller forwards the SAME
            // clear, then calls AfterClear; failed clears are rolled back before native drawing.
            if (FAILED(set(owner_, surface_.Get()))) { status_ = DepthStatus::Bind; return; }
            native_ = native;
            redirected_ = pending_ = true;
        }
        bool AfterClear(HRESULT result)
        {
            if (!pending_) return false;
            pending_ = false;
            if (SUCCEEDED(result)) { valid_ = true; status_ = DepthStatus::Ready; }
            else
            {
                valid_ = false; status_ = DepthStatus::ClearFailed;
                return redirected_ && Restore(); // caller retries this failed clear on the native surface
            }
            return false;
        }
        HRESULT Set(Surface* surface)
        {
            return set(owner_, redirected_ && surface == native_.Get() ? surface_.Get() : surface);
        }
        // The client keeps seeing its own surface identity, including a real AddRef.
        HRESULT Get(Surface** out)
        {
            const HRESULT result = get(owner_, out);
            if (SUCCEEDED(result) && out && redirected_ && !(readable_ && multisampled_) && *out == surface_.Get())
            {
                (*out)->Release();
                *out = native_.Get();
                (*out)->AddRef();
            }
            return result;
        }
        bool Owns(Device* device) const { return active_ && device == owner_; }
        bool BeginRead()
        {
            if (!valid_ || readable_ || (!redirected_ && !multisampled_)) return false;
            saved_.Reset();
            const HRESULT binding = get(owner_, saved_.GetAddressOf());
            if (FAILED(binding) && binding != D3DERR_NOTFOUND)
            { status_ = DepthStatus::ReadFailed; return false; }
            if(multisampled_) {
                if(saved_.Get()!=(redirected_ ? surface_.Get() : native_.Get())) { saved_.Reset(); status_=DepthStatus::ReadFailed; return false; }
                readable_=true; return true;
            }
            // Always detach depth for ordered screen-space effects, including native surfaces
            // other than ours. Preserve the exact binding (including null) across that interval.
            if (FAILED(set(owner_, nullptr)))
            { saved_.Reset(); status_ = DepthStatus::ReadFailed; return false; }
            readable_ = true;
            return true;
        }
        bool EndRead()
        {
            if (!readable_) return true;
            if (FAILED(set(owner_, saved_.Get())))
            { valid_ = false; status_ = DepthStatus::RestoreFailed; return false; }
            readable_ = false; saved_.Reset();
            return true;
        }
        bool End()
        {
            valid_ = false;
            if (!EndRead() || !Restore()) return false;
            if(multisampled_) { native_.Reset(); multisampled_=false; }
            active_ = false; pending_ = false;
            return true;
        }
        void Reset()
        {
            // Reset must release every owned DEFAULT-pool reference even when the lost driver
            // rejects restoration. No native drawing is performed until its Reset completes.
            if (!End() && owner_ && set) set(owner_, nullptr);
            saved_.Reset(); native_.Reset();
            active_ = redirected_ = pending_ = valid_ = readable_ = multisampled_ = false;
            ReleaseTexture(); owner_ = nullptr;
        }
        Texture* ReadTexture() const { return readable_ && valid_ ? texture_.Get() : nullptr; }
        Surface* ReadMultisampledSurface() const { return readable_ && valid_ && multisampled_ ? (redirected_ ? surface_.Get() : native_.Get()) : nullptr; }
        bool Multisampled() const { return multisampled_; }
        bool Valid() const { return valid_; }
        UINT Width() const { return width_; }
        UINT Height() const { return height_; }
        const D3DVIEWPORT9& Viewport() const { return viewport_; }
        const D3DSURFACE_DESC& ColorDesc() const { return colorDesc_; }
        const D3DSURFACE_DESC& DepthDesc() const { return depthDesc_; }
        DepthStatus Status() const { return status_; }
    private:
        bool Restore()
        {
            if (!redirected_) return true;
            Ptr<Surface> current;
            const HRESULT binding = get(owner_, current.GetAddressOf());
            if ((FAILED(binding) && binding != D3DERR_NOTFOUND) ||
                (current.Get() == surface_.Get() && FAILED(set(owner_, native_.Get()))))
            { status_ = DepthStatus::RestoreFailed; return false; }
            redirected_ = false; native_.Reset();
            return true;
        }
        void ReleaseTexture()
        {
            surface_.Reset(); texture_.Reset(); width_ = height_ = 0;
        }
        Device* owner_ = nullptr;
        Ptr<Texture> texture_;
        Ptr<Surface> surface_, native_, saved_;
        UINT width_ = 0, height_ = 0;
        D3DFORMAT colorFormat_ = D3DFMT_UNKNOWN;
        D3DVIEWPORT9 viewport_{};
        D3DSURFACE_DESC colorDesc_{}, depthDesc_{};
        bool active_ = false, attempted_ = false, redirected_ = false;
        bool pending_ = false, valid_ = false, readable_ = false, multisampled_ = false;
        DepthStatus status_ = DepthStatus::Waiting;
    };
}
