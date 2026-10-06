// Texture create/upload detours: publish BLP-load / texture-upload events and guard the mip-source singleton.
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
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "engine/diag/AssetProfile.hpp"
#include "engine/render/UiTextureAudit.hpp"

#include "common/Log.hpp"
#include "offsets/engine/Gx.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace
{
    namespace ev    = wxl::events;
    namespace gxoff = wxl::offsets::engine::gx;
    namespace aprof = wxl::runtime::assetprof;

    gxoff::TextureCreateFn g_origTexCreate = nullptr;
    gxoff::TextureUpdateFn g_origTexUpdate = nullptr;
    gxoff::BlpLockChain2Fn g_origBlpLockChain2 = nullptr;
    std::atomic<uint32_t>  g_textureUpdateFaults{ 0 };

    // Native callback at 4B5E80: eight cdecl arguments, plain ret. Phase 0
    // reloads the BLP if the shared chain is not live; phase 1 returns pitch
    // and the selected mip pointer. Verified against the installed client.
    using TextureSourceFn = void(__cdecl*)(unsigned, unsigned, unsigned,
        unsigned, unsigned, void*, unsigned*, void**);
    TextureSourceFn g_origTextureSource = nullptr;
    void __cdecl hkTextureSource(unsigned phase, unsigned width, unsigned height,
        unsigned face, unsigned mip, void* handle, unsigned* pitch, void** data)
    {
        const unsigned before=*reinterpret_cast<unsigned*>(gxoff::kMipTableValid);
        g_origTextureSource(phase,width,height,face,mip,handle,pitch,data);
        static unsigned reports=0;
        static void* lastDevice=nullptr;
        const auto gx=*reinterpret_cast<uintptr_t*>(gxoff::kGxDevicePtr);
        void* device=gx?*reinterpret_cast<void**>(gx+gxoff::kD3DDeviceField):nullptr;
        if(device!=lastDevice) { lastDevice=device; reports=0; }
        if(reports>=1024 || !handle || (phase!=0 && phase!=1) || mip) return;
        const char* name=static_cast<const char*>(handle)+gxoff::kTexHandleNameField;
        if(_strnicmp(name,"Interface\\",10)!=0) return;
        ++reports;
        // Only fingerprint the same bounded DXT3/5-shaped sample as the
        // binding audit. Other UI requests still report source availability.
        uint64_t hash=14695981039346656037ull;
        unsigned bytes=0;
        if(phase==1 && width==128 && height==32 && pitch && *pitch==512 && data && *data) {
            const auto* source=static_cast<const unsigned char*>(*data);
            for(unsigned i=0;i<4096;++i) { hash^=source[i]; hash*=1099511628211ull; }
            bytes=4096;
        }
        WLOG_INFO("ui-texture-source: phase=%u device=%p name=%s handle=%p size=%ux%u mip=%u valid=%u/%u source=%p pitch=%u bytes=%u fnv=%016llX",
            phase,device,name,handle,width,height,mip,before,
            *reinterpret_cast<unsigned*>(gxoff::kMipTableValid),
            phase==1 && data?*data:nullptr,phase==1 && pitch?*pitch:0,bytes,hash);
    }

    // Keep SEH in a POD-only leaf. TextureUpdate invokes the texture's completion callback before it
    // returns; a late font-atlas/cache callback can retain a row/tree pointer whose owner was rebuilt
    // during world entry. Letting that AV escape kills the client from inside the texture completion callback.
    bool SafeTextureUpdate(
        void* texture, int x, int y, int x2, int y2, int flag) noexcept
    {
        __try
        {
            g_origTexUpdate(texture, x, y, x2, y2, flag);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    /**
     * @brief Detours a texture update request, emitting OnTextureUpload before it is queued.
     *
     * The native entry receives the texture and a rectangle as six cdecl arguments. The detour must
     * forward all six: calling the trampoline with only the texture shifts its remaining parameters
     * onto unrelated stack data and eventually corrupts or skips icon uploads.
     *
     * GxTexUpdate only marks the texture for a later device update. The mip source consumed by that
     * deferred work is a process-wide singleton (kMipTablePtr is a pointer whose buffer holds the
     * per-mip source pointers; kMipTableValid gates the read). A build fills it with raw aliases into
     * its transient IO buffer, then queues the update. Two ways that singleton turns into an
     * access-violation use-after-free are handled without clearing it here:
     *  - a NESTED build run while this upload is mid-copy overwrites the table and frees its buffer; the
     *    async-drain serializer (streaming) snapshots and restores the table around the nested build it
     *    runs, so the outer upload keeps reading its own live aliases.
     *  - a TRUNCATED mip chain (common in custom-map BLPs) only fills the low slots. The LockChain2
     *    detour below clears the singleton before the next build repopulates it, leaving unfilled high
     *    slots at zero without destroying sources still needed by a queued update. Clearing here caused
     *    gxRestart/window-resize recreation to produce black character skins and white UI textures.
     * @param texture texture being uploaded.
     * @param x upload rectangle left.
     * @param y upload rectangle top.
     * @param x2 upload rectangle right.
     * @param y2 upload rectangle bottom.
     * @param flag native upload flag.
     */
    void __cdecl hkTexUpdate(
        void* texture, int x, int y, int x2, int y2, int flag)
    {
        const uint32_t width = x2 > x ? static_cast<uint32_t>(x2 - x) : 0;
        const uint32_t height = y2 > y ? static_cast<uint32_t>(y2 - y) : 0;
        ev::TextureUploadArgs a{ texture, width, height };
        ev::Emit(ev::Event::OnTextureUpload, &a);
        const uint64_t started = aprof::Now();
        const bool completed = SafeTextureUpdate(texture, x, y, x2, y2, flag);
        if (!completed)
        {
            const uint32_t faults = g_textureUpdateFaults.fetch_add(1, std::memory_order_relaxed) + 1;
            if (faults == 1 || (faults & (faults - 1)) == 0)
                WLOG_WARN(
                    "texture: skipped stale native upload callback "
                    "(faults=%u texture=%p rect=%d,%d..%d,%d)",
                    faults, texture, x, y, x2, y2);
        }
        else if (started)
        {
            aprof::Record(
                aprof::Phase::TextureUpload, aprof::Now() - started,
                static_cast<uint64_t>(width) * height);
        }
    }

    /**
     * @brief Clears stale singleton mip slots immediately before the native BLP chain is rebuilt.
     *
     * LockChain2 reconstructs every valid pointer for this build. Scoping the clear to the stock
     * global chain owner preserves unrelated MippedImg chains and prevents truncated files from
     * inheriting high mip pointers from the preceding texture.
     */
    int __fastcall hkBlpLockChain2(
        void* blp, void* /*edx*/, uint32_t source, int format,
        uint32_t** chainOwner, uint32_t firstMip, int direct)
    {
        if (chainOwner &&
            reinterpret_cast<uintptr_t>(chainOwner) == gxoff::kMipTablePtr &&
            *chainOwner)
        {
            std::memset(*chainOwner, 0,
                        gxoff::kMipTableSlots * sizeof(uint32_t));
        }
        return g_origBlpLockChain2(
            blp, nullptr, source, format, chainOwner, firstMip, direct);
    }

    /**
     * @brief Detours the by-name texture create, emitting OnBlpLoad after the request resolves.
     *
     * Fires on every reference (returns the cached handle on a hit), so the event carries the requested
     * name and a subscriber can watch for one specific BLP.
     * @param name    requested texture path (full virtual path).
     * @param flags   native load flags.
     * @param status  native status out-pointer.
     * @param flags2  native load flags.
     * @return the resolved texture handle (null on failure).
     */
    void* __cdecl hkTexCreate(const char* name, uint32_t flags, int* status, uint32_t flags2)
    {
        const uint64_t started = aprof::Now();
        void* handle = g_origTexCreate(name, flags, status, flags2);
        if (wxl::render::uiaudit::Enabled() && name &&
            (_strnicmp(name,"Interface\\Buttons\\UI-Panel-Button-",34)==0))
        {
            static unsigned reports=0;
            if(reports++<128) WLOG_INFO("ui-texture-request: name=%s handle=%p status=%d flags=%08X/%08X",
                name,handle,status?*status:-1,flags,flags2);
        }
        if (started) aprof::Record(aprof::Phase::TextureRequest, aprof::Now() - started);

        ev::BlpLoadArgs a{ name, handle };
        ev::Emit(ev::Event::OnBlpLoad, &a);

        return handle;
    }

    bool InstallTextures()
    {
        if(wxl::render::uiaudit::Enabled())
            wxl::hook::Install("TextureSourceAudit", 0x004B5E80,
                &hkTextureSource, &g_origTextureSource);
        wxl::hook::Install("CBLPFile.LockChain2", gxoff::kBlpLockChain2,
                           &hkBlpLockChain2, &g_origBlpLockChain2);
        wxl::hook::Install("TextureUpdate", gxoff::kTextureUpdate, &hkTexUpdate, &g_origTexUpdate);
        wxl::hook::Install("TextureCreate", gxoff::kTextureCreate, &hkTexCreate, &g_origTexCreate);
        return true;
    }
}

WXL_REGISTER_FEATURE("textures", true, InstallTextures)
