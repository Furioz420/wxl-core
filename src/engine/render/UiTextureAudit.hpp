// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include "common/Log.hpp"

namespace wxl::render::uiaudit {
inline bool Enabled() {
    static const bool enabled=[] { char value[8]{};
        return GetEnvironmentVariableA("WXL_UI_TEXTURE_AUDIT",value,sizeof(value))==1 && value[0]=='1'; }();
    return enabled;
}
// A representative failing UI asset is 128x32. Size is a sample filter, NOT
// attribution to a filename. FNV of tight compressed rows can be compared with
// extracted BLP mip0. This reads managed backing bytes, not final GPU pixels.
inline void Observe(IDirect3DDevice9* device,DWORD stage,IDirect3DBaseTexture9* base,HRESULT bound) {
    static unsigned reports=0;
    if(!Enabled() || reports>=128 || stage || !base || base->GetType()!=D3DRTYPE_TEXTURE) return;
    auto* texture=static_cast<IDirect3DTexture9*>(base);
    D3DSURFACE_DESC desc{};
    if(FAILED(texture->GetLevelDesc(0,&desc)) || desc.Width!=128 || desc.Height!=32) return;
    struct Seen {void* pointer=nullptr; DWORD at=0;};
    static Seen seen[64]{}; static unsigned next=0;
    const DWORD now=GetTickCount();
    for(auto& entry:seen) if(entry.pointer==base) {
        if(now-entry.at<5000) return;
        entry.pointer=nullptr; break;
    }
    seen[next++%64]={base,now}; ++reports;
    IDirect3DDevice9* owner=nullptr;
    const HRESULT ownerResult=texture->GetDevice(&owner);
    uint64_t hash=14695981039346656037ull;
    HRESULT read=S_FALSE; unsigned bytes=0;
    const unsigned block=desc.Format==D3DFMT_DXT1?8:
        (desc.Format==D3DFMT_DXT3 || desc.Format==D3DFMT_DXT5)?16:0;
    if(desc.Pool==D3DPOOL_MANAGED && block) {
        D3DLOCKED_RECT lock{};
        read=texture->LockRect(0,&lock,nullptr,D3DLOCK_READONLY);
        if(SUCCEEDED(read)) {
            const unsigned rowBytes=32*block;
            if(lock.pBits && lock.Pitch>=int(rowBytes)) {
                for(unsigned y=0;y<8;++y) for(unsigned x=0;x<rowBytes;++x) {
                    hash^=static_cast<const unsigned char*>(lock.pBits)[y*lock.Pitch+x];
                    hash*=1099511628211ull;
                }
                bytes=rowBytes*8;
            } else read=E_FAIL;
            const HRESULT unlocked=texture->UnlockRect(0);
            if(FAILED(unlocked)) read=unlocked;
        }
    }
    WLOG_INFO("ui-texture-audit: sample=%u device=%p texture=%p owner=%p ownerHr=%08lX bound=%08lX format=%08lX pool=%u read=%08lX bytes=%u fnv=%016llX",
        reports,device,base,owner,ownerResult,bound,DWORD(desc.Format),unsigned(desc.Pool),read,bytes,hash);
    if(owner) owner->Release();
}
}
