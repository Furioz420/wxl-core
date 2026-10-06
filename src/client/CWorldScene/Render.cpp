#include "engine/render/FrameFlashTrace.hpp"
// Native D3D9 render feature: device-vtable + engine detours that publish the render events.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "config.hpp"
#include "common/Mem.hpp"
#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "engine/render/RenderService.hpp"
#include "engine/render/ScreenEffectsBridge.hpp"
#include "engine/render/TransparentFogProbe.hpp"
#include "engine/render/NativeWorldScope.hpp"
#include "wxl/RenderDefaults.hpp"
#include "engine/render/DepthService.hpp"
#include "engine/render/UiTextureAudit.hpp"
#include "game/Gx.hpp"
#include "offsets/engine/Gx.hpp"

#include <windows.h>
#include <d3d9.h>
#include <intrin.h>

// Uses device-vtable pointer swaps (lifecycle and opt-in depth interception) plus hooks on the
// liquid/world-boundary paths; no mid-function inline patch, so the native render pass stays intact.
// The detours publish events and bracket shared frame inputs/ordered effects; the client owns
// its native D3D9 rendering. No second proxy or draw-hook owner is installed.
//
// DrawIndexedPrimitive is NOT swapped here: that vtable slot, and the one-shot draw interceptor built
// on it, belong to wxl-m2 (its per-batch OnM2BatchDraw/OnRibbonDraw need the same slot; a second core
// swap on top would just fight it for the same vtable entry). wxl-m2 re-applies its own swap on the
// same per-device-recreate cadence this file uses, and publishes "wxl.m2draw" for any
// other extension (wxl-wmo's four-layer material) that needs to bracket one native draw -- see
// include/wxl/M2DrawApi.h.
namespace
{
    namespace off = wxl::offsets::engine::gx;
    namespace ev  = wxl::events;
    namespace gx  = wxl::game::gx;

    using EndSceneFn = long (__stdcall*)(void*);
    using PresentFn  = long (__stdcall*)(void*, const void*, const void*, void*, const void*);
    using ResetFn    = long (__stdcall*)(void*, D3DPRESENT_PARAMETERS*);

    EndSceneFn g_origEndScene = nullptr;
    PresentFn  g_origPresent  = nullptr;
    ResetFn    g_origReset    = nullptr;
    wxl::render::depth::GetFn g_origGetDepth = nullptr;
    wxl::render::depth::SetFn g_origSetDepth = nullptr;
    using ClearFn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
    ClearFn g_origClear = nullptr;
    using SetTextureFn = HRESULT(__stdcall*)(IDirect3DDevice9*,DWORD,IDirect3DBaseTexture9*);
    SetTextureFn g_origSetTexture = nullptr;
    HRESULT __stdcall hkSetTexture(IDirect3DDevice9* device,DWORD stage,IDirect3DBaseTexture9* texture)
    {
        const HRESULT result=g_origSetTexture(device,stage,texture);
        wxl::render::uiaudit::Observe(device,stage,texture,result);
        return result;
    }
    off::WorldRenderFinalizeFn g_origWorldFinalize = nullptr;
    off::WorldOnRenderFn       g_origWorldScene    = nullptr;
    off::LiquidRenderPassFn    g_origLiquidRender  = nullptr;
    unsigned g_worldNesting = 0;
    using ScreenEffectsFn=void(__cdecl*)();
    ScreenEffectsFn g_origScreenEffects=nullptr;
    void __cdecl BeforeScreenEffects(uintptr_t caller) {
        if(g_worldNesting==1 && caller==0x004F9286) wxl::render::BeforeScreenEffects(gx::RawDevice());
    }
    // Preserve general registers, flags, x87 and SSE state before tail-calling
    // the native trampoline. The original stack and all native arguments survive.
    WXL_SCREEN_EFFECTS_BRIDGE(hkScreenEffects,BeforeScreenEffects,g_origScreenEffects)
    bool ScreenEffectsLayout() {
        __try {
            const auto* base=reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
            if(reinterpret_cast<uintptr_t>(base)!=0x00400000) return false;
            const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x1000) return false;
            const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
            if(pe->Signature!=IMAGE_NT_SIGNATURE || pe->FileHeader.TimeDateStamp!=0x4C2452FE) return false;
            constexpr uintptr_t site=0x004F9281,target=0x008C1010;
            const unsigned char prefix[]{0x55,0x8B,0xEC,0xA1,0x74,0x57,0xD4,0x00};
            for(unsigned i=0;i<sizeof(prefix);++i) if(reinterpret_cast<const unsigned char*>(target)[i]!=prefix[i]) return false;
            return *reinterpret_cast<const unsigned char*>(site)==0xE8 &&
                site+5+*reinterpret_cast<const int32_t*>(site+1)==target;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Bounded diagnostics for settings transitions that can recreate a swap chain
    // without entering Reset. Successful world draws alone do not prove presentation.
    void ReportPresentation(IDirect3DDevice9* device, HRESULT result)
    {
        static DWORD lastReport = 0;
        static HRESULT previous = S_OK;
        static unsigned reports = 0, frames = 0, failed = 0;
        ++frames;
        if (FAILED(result)) ++failed;
        const DWORD now = GetTickCount();
        if (reports >= 60 || (reports && result == previous && now - lastReport < 10000)) return;
        previous = result;
        lastReport = now;
        ++reports;
        IDirect3DSurface9* back = nullptr;
        IDirect3DSurface9* target = nullptr;
        IDirect3DSwapChain9* chain = nullptr;
        D3DSURFACE_DESC backDesc{}, targetDesc{};
        D3DPRESENT_PARAMETERS params{};
        const HRESULT backResult = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
        const HRESULT targetResult = device->GetRenderTarget(0, &target);
        if (back) back->GetDesc(&backDesc);
        if (target) target->GetDesc(&targetDesc);
        HRESULT chainResult = device->GetSwapChain(0, &chain);
        if (chain) chainResult = chain->GetPresentParameters(&params);
        WLOG_INFO("render-present-v1: dev=%p hr=0x%08lX calls=%u failed=%u back=%ux%u aa=%u target=%ux%u aa=%u same=%u window=%p windowed=%u queries=0x%08lX/0x%08lX/0x%08lX",
            static_cast<void*>(device), static_cast<unsigned long>(result), frames, failed,
            backDesc.Width, backDesc.Height, static_cast<unsigned>(backDesc.MultiSampleType),
            targetDesc.Width, targetDesc.Height, static_cast<unsigned>(targetDesc.MultiSampleType),
            back && back == target ? 1u : 0u, static_cast<void*>(params.hDeviceWindow),
            static_cast<unsigned>(params.Windowed), static_cast<unsigned long>(backResult),
            static_cast<unsigned long>(targetResult), static_cast<unsigned long>(chainResult));
        if (chain) chain->Release();
        if (target) target->Release();
        if (back) back->Release();
        frames = failed = 0;
    }
    HRESULT __stdcall hkSetDepth(IDirect3DDevice9* device, IDirect3DSurface9* surface)
    { return wxl::render::depth::Set(device, surface); }
    HRESULT __stdcall hkGetDepth(IDirect3DDevice9* device, IDirect3DSurface9** out)
    { return wxl::render::depth::Get(device, out); }
    HRESULT __stdcall hkClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects,
                             DWORD flags, D3DCOLOR color, float z, DWORD stencil)
    {
        wxl::render::depth::BeforeClear(device, count, rects, flags, z, g_worldNesting == 1, _ReturnAddress());
        const HRESULT result = g_origClear(device, count, rects, flags, color, z, stencil);
        if (wxl::render::depth::AfterClear(device, result))
            return g_origClear(device, count, rects, flags, color, z, stencil);
        return result;
    }
    void EnsureDeviceHooks(IDirect3DDevice9* dev);   // defined after SwapVtbl

    /**
     * @brief Reconciles device hooks from the logic cadence, which continues while rendering is lost.
     *
     * WorldFinalize normally supplies the cheapest per-frame check, but it is never reached once WoW's
     * graphics context is down.  A driver may replace the device vtable before the engine's recovery Reset;
     * in that case Reset and Present both bypass WXL and the client can remain visually frozen while input,
     * audio and simulation continue. OnUpdate runs on that still-live main loop and restores the hooks in
     * time for the next native recovery attempt.
     */
    void ReconcileDeviceHooks(void*, const void*)
    {
        if (IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(gx::RawDevice()))
            EnsureDeviceHooks(d);
    }

    /** @brief Re-arms the D3D hooks before WoW's native window procedure handles a restore/reset message. */
    void ReconcileDeviceHooksOnInput(void*, const void* rawArgs)
    {
        const auto* args = static_cast<const ev::InputArgs*>(rawArgs);
        if (!args) return;
        switch (args->message)
        {
            case WM_SIZE:
            case WM_ACTIVATE:
            case WM_ACTIVATEAPP:
            case WM_SETFOCUS:
            case WM_DISPLAYCHANGE:
            case WM_SYSCOMMAND:
                ReconcileDeviceHooks(nullptr, nullptr);
                break;
            default:
                break;
        }
    }

    /**
     * @brief Detours EndScene, emitting OnEndScene before the native call.
     * @param dev  D3D9 device.
     * @return the EndScene result.
     */
    long __stdcall hkEndScene(void* dev)
    {
        ev::EndSceneArgs a{ dev };
        ev::Emit(ev::Event::OnEndScene, &a);
        return g_origEndScene(dev);
    }

    /**
     * @brief Detours Present, emitting OnFrame just before the buffers swap.
     * @param dev    D3D9 device.
     * @param src    source rect.
     * @param dst    destination rect.
     * @param wnd    target window override.
     * @param dirty  dirty region.
     * @return the Present result.
     */
    long __stdcall hkPresent(void* dev, const void* src, const void* dst, void* wnd, const void* dirty)
    {
        wxl::render::EndWorld();
        ev::FrameArgs a{ dev };
        ev::Emit(ev::Event::OnFrame, &a);
        wxl::render::flashtrace::Sample(static_cast<IDirect3DDevice9*>(dev),3);
        const auto result = g_origPresent(dev, src, dst, wnd, dirty);
        wxl::render::flashtrace::Presented(result,static_cast<IDirect3DDevice9*>(dev));
        ReportPresentation(static_cast<IDirect3DDevice9*>(dev), result);
        return result;
    }

    /**
     * @brief Detours the per-frame liquid render pass, emitting OnLiquidRender before the native draw.
     *
     * Both native buckets (passType 0 and 1) can draw world liquids. The liquid textures are bound and the
     * wave animation already applied at this point.
     * @param bank       liquid material-settings bank (this-in-ECX), indexed by passType.
     * @param edx        unused register slot for the thiscall convention.
     * @param transform  shared liquid transform forwarded to every instance draw.
     * @param passType   native liquid bucket index (0 or 1).
     */
    void __fastcall hkLiquidRender(void* bank, void* edx, void* transform, int passType)
    {
        const uint32_t count = bank && passType >= 0 && passType <= 1
            ? static_cast<off::LiquidPassEntry*>(bank)[passType].count : 0;
        wxl::render::BeforeLiquids(passType, count, g_worldNesting == 1);

        ev::LiquidRenderArgs a{ bank, transform, passType, count };
        ev::Emit(ev::Event::OnLiquidRender, &a);

        if(g_worldNesting==1 && count && ev::Any(ev::Event::OnLiquidRenderBegin)) {
            wxl::render::depth::CaptureWaterDepth(true);
            wxl::render::depth::BeginLiquidRead();
            ev::Emit(ev::Event::OnLiquidRenderBegin, &a);
            wxl::render::depth::DiscardLiquidDepth();
        }
        g_origLiquidRender(bank, edx, transform, passType);
        if(g_worldNesting==1 && count && ev::Any(ev::Event::OnLiquidRenderEnd)) {
            wxl::render::depth::CaptureWaterDepth(true);
            wxl::render::depth::BeginLiquidRead();
            ev::Emit(ev::Event::OnLiquidRenderEnd, &a);
            wxl::render::depth::DiscardLiquidDepth();
        }
    }

    /**
     * @brief Detours the world scene pass, emitting OnWorldSceneEnd once it has drawn.
     *
     * Its caller runs this, then the world text batch, then puts back the projection and view it saved
     * before the pass. Emitting on the way out of the pass therefore lands in the one window where the
     * world is complete and the matrices that drew it are still the ones on the device -- which is what
     * a subscriber placing geometry by world coordinate needs, and what the later world -> UI boundary
     * no longer offers.
     * @param worldFrame  world frame being rendered.
     * @param edx         unused; the register the native convention passes nothing meaningful in.
     */
    void __fastcall hkWorldScene(void* worldFrame, void* edx)
    {
        const bool outerWorld = g_worldNesting++ == 0;
        if (outerWorld) { wxl::render::BeginWorld(gx::RawDevice()); wxl::render::transparent::Begin(); }
        // Taken before the pass, not after: the post-process passes run inside it and can leave a
        // surface of their own bound. A subscriber that depth-tests against whatever it finds bound
        // afterwards is testing against a surface the world never wrote to, which rejects all of its
        // geometry and reports nothing.
        IDirect3DSurface9* sceneDepth = nullptr;
        if (IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(gx::RawDevice()))
            d->GetDepthStencilSurface(&sceneDepth);

        struct WorldCall { void* frame; void* edx; bool outer; } call{worldFrame,edx,outerWorld};
        wxl::render::RunNativeWorld(
            [](void* p) { if(static_cast<WorldCall*>(p)->outer) wxl::render::NativeWorldBegin(gx::RawDevice()); },
            [](void* p) { auto& c=*static_cast<WorldCall*>(p); g_origWorldScene(c.frame,c.edx); },
            [](void* p) { if(static_cast<WorldCall*>(p)->outer) ev::Emit(ev::Event::OnNativeWorldEnd,nullptr); },&call);

        if (outerWorld) wxl::render::depth::SaveCamera();

        if (ev::Any(ev::Event::OnWorldSceneEnd))
        {
            ev::WorldSceneEndArgs a{ gx::RawDevice(), sceneDepth };
            ev::Emit(ev::Event::OnWorldSceneEnd, &a);
        }

        if (sceneDepth) sceneDepth->Release();
        if(outerWorld) wxl::render::transparent::End();
        --g_worldNesting;
    }

    /**
     * @brief Detours world-frame finalize: the world -> UI boundary. Re-applies the device vtable hooks on
     *        the live device (they are lost on a device recreate, and this function-entry hook survives it),
     *        runs the native finalize, then emits OnWorldRenderEnd for post-world subscribers.
     * @param worldFrame  world frame being finalized.
     */
    void __cdecl hkWorldFinalize(void* worldFrame)
    {
        if (IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(gx::RawDevice()))
            EnsureDeviceHooks(d);

        g_origWorldFinalize(worldFrame);

        ev::WorldRenderEndArgs a{ gx::RawDevice() };
        if (g_worldNesting == 0) wxl::render::Dispatch(gx::RawDevice());
        ev::Emit(ev::Event::OnWorldRenderEnd, &a);
        if (g_worldNesting == 0) wxl::render::EndWorld();
    }

    /**
     * @brief Replaces one vtable entry with a hook, returning the original through origOut.
     * @param vtbl     vtable base.
     * @param idx      entry index to swap.
     * @param hook     replacement function pointer.
     * @param origOut  receives the original entry.
     */
    template <class Fn>
    void SwapVtbl(void** vtbl, unsigned idx, Fn* hook, Fn** origOut)
    {
        wxl::mem::SwapPointer(&vtbl[idx], reinterpret_cast<void*>(hook),
                              reinterpret_cast<void**>(origOut));
    }

    /**
     * @brief Detours IDirect3DDevice9::Reset (a resolution / window resize). D3D9 requires every
     *        D3DPOOL_DEFAULT resource released before a Reset or it fails (D3DERR_INVALIDCALL) and the device
     *        is lost -- the resize "crash". Fires OnDeviceLost so subscribers retire their GPU work and free
     *        DEFAULT-pool resources, releases the engine's own tracked render targets, runs the native Reset,
     *        then fires OnDeviceReset on success so subscribers recreate.
     * @param dev     the D3D9 device being reset.
     * @param params  the present parameters the device resets with (new size / window mode).
     * @return the native Reset result.
     */
    long __stdcall hkReset(void* dev, D3DPRESENT_PARAMETERS* params)
    {
        static unsigned resetLog = 0;
        const bool logThis = resetLog < 16;
        if (logThis)
        {
            ++resetLog;
            WLOG_INFO("render: Reset begin %ux%u windowed=%u",
                      params ? params->BackBufferWidth : 0,
                      params ? params->BackBufferHeight : 0,
                      params ? static_cast<unsigned>(params->Windowed) : 0);
        }

        ev::DeviceResetArgs a{ dev, params };
        // Preserve the recovery order from wxl-core PR #5: retire core-owned DEFAULT-pool render
        // targets first, then let extensions release their own resources, and only then enter the
        // native Reset.  In particular, subscribers are allowed to inspect/reset their own state;
        // none of them should observe a still-live shared render target while doing so.
        wxl::render::transparent::Cancel();
        wxl::render::DeviceLost();
        gx::ReleaseResetResources();
        if (logThis) WLOG_INFO("render: Reset tracked-target cleanup complete");
        if (logThis) WLOG_INFO("render: Reset notifying device-lost subscribers");
        ev::Emit(ev::Event::OnDeviceLost, &a);
        if (logThis) WLOG_INFO("render: Reset subscriber cleanup complete");

        const long r = g_origReset(dev, params);

        // D3D9 may restore the device's original vtable in-place during Reset.  The COM object address
        // stays unchanged, so a device-pointer-only guard mistakes the now-unhooked device for one that
        // is still hooked.  Reconcile the live slots immediately, even when Reset failed: otherwise the
        // next recovery Reset bypasses WXL cleanup and WoW can remain forever at hasContext=0 while its
        // input, audio and simulation threads continue normally.
        EnsureDeviceHooks(static_cast<IDirect3DDevice9*>(dev));

        if (SUCCEEDED(r))
        {
            ev::Emit(ev::Event::OnDeviceReset, &a);
            if (logThis) WLOG_INFO("render: Reset ok");
        }
        else
        {
            static unsigned logged = 0;
            if (logged < 8)
            {
                ++logged;
                WLOG_WARN("render: Reset failed hr=0x%08lX", static_cast<unsigned long>(r));
            }
        }
        return r;
    }

    /**
     * @brief Installs the render vtable hooks on the live device. Each device instance may carry its own
     *        vtable, so on a device recreate the swaps are gone; this re-applies them on the current device.
     *        The surviving world-render function-entry hook calls this each frame, so OnEndScene / OnFrame /
     *        OnM2BatchDraw keep firing after a graphics restart. Guarded against a shared vtable: if it already
     *        carries our hooks, re-swapping would capture our own hook as the "original" and recurse, so only
     *        the device pointer is updated.
     * @param dev  the live D3D9 device.
     */
    void EnsureDeviceHooks(IDirect3DDevice9* dev)
    {
        if (!dev) return;
        void** vtbl = *reinterpret_cast<void***>(dev);
        bool installed = false;
        if (wxl::render::uiaudit::Enabled() && vtbl[off::vt::kSetTexture] != reinterpret_cast<void*>(&hkSetTexture))
        {
            SwapVtbl(vtbl, off::vt::kSetTexture, &hkSetTexture, &g_origSetTexture);
            installed = true;
        }
        if (vtbl[off::vt::kGetDepthStencil] != reinterpret_cast<void*>(&hkGetDepth))
        {
            SwapVtbl(vtbl, off::vt::kGetDepthStencil, &hkGetDepth, &g_origGetDepth);
            installed = true;
        }
        if (vtbl[off::vt::kSetDepthStencil] != reinterpret_cast<void*>(&hkSetDepth))
        {
            SwapVtbl(vtbl, off::vt::kSetDepthStencil, &hkSetDepth, &g_origSetDepth);
            installed = true;
        }
        wxl::render::depth::Configure(g_origGetDepth, g_origSetDepth);
        if (vtbl[off::vt::kClear] != reinterpret_cast<void*>(&hkClear))
        {
            SwapVtbl(vtbl, off::vt::kClear, &hkClear, &g_origClear);
            installed = true;
        }
        if (vtbl[off::vt::kEndScene] != reinterpret_cast<void*>(&hkEndScene))
        {
            SwapVtbl(vtbl, off::vt::kEndScene, &hkEndScene, &g_origEndScene);
            installed = true;
        }
        if (vtbl[off::vt::kPresent] != reinterpret_cast<void*>(&hkPresent))
        {
            SwapVtbl(vtbl, off::vt::kPresent, &hkPresent, &g_origPresent);
            installed = true;
        }
        if (vtbl[off::vt::kReset] != reinterpret_cast<void*>(&hkReset))
        {
            SwapVtbl(vtbl, off::vt::kReset, &hkReset, &g_origReset);
            installed = true;
        }
        if (installed)
            WLOG_INFO("render: device hooks installed (dev=%p)", (void*)dev);
    }

    /**
     * @brief Installs the render detours: device vtable swaps plus the world-boundary / liquid
     *        function-entry hooks. Detours are enabled by the caller's batch EnableAll() afterwards.
     * @return true; a missing device only defers the vtable swaps to the first world finalize.
     */
    bool Install()
    {
        if (void* dev = gx::RawDevice())
            EnsureDeviceHooks(static_cast<IDirect3DDevice9*>(dev));
        else
            WLOG_WARN("render: device not up, vtable hooks deferred to first world finalize");

        wxl::hook::Install("WorldRenderFinalize", off::kWorldRenderFinalize,
                           &hkWorldFinalize, &g_origWorldFinalize);
        wxl::hook::Install("WorldScenePass", off::kWorldOnRender,
                           &hkWorldScene, &g_origWorldScene);
        wxl::hook::Install("LiquidRenderPass", off::kLiquidRenderPass,
                           &hkLiquidRender, &g_origLiquidRender);
        if(wxl::render::defaults::Enabled("WXL_RENDER_EARLY_FOG") && ScreenEffectsLayout())
            wxl::hook::Install("BeforeScreenEffects",0x008C1010,&hkScreenEffects,&g_origScreenEffects);
        else WLOG_WARN("render: screen-effects boundary unavailable; late atmosphere retained");

        wxl::render::transparent::InstallProbe();
        ev::Subscribe(ev::Event::OnUpdate, &ReconcileDeviceHooks, nullptr);
        ev::Subscribe(ev::Event::OnInput, &ReconcileDeviceHooksOnInput, nullptr);
        ev::Subscribe(ev::Event::OnInput, &wxl::render::depth::OnInput, nullptr);
        ev::Subscribe(ev::Event::OnInput, &wxl::render::OnComparisonInput, nullptr);

        WLOG_INFO("render: hooks installed (EndScene, Present, Reset, Clear, Get/SetDepth, WorldFinalize, WorldScenePass, LiquidRenderPass)");
        return true;
    }
}

WXL_REGISTER_FEATURE("render", true, Install)
