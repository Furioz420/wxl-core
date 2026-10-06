// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <unordered_map>

namespace wxl::input
{
    // Keep each window's original independently: the old window can still
    // receive teardown messages after its replacement has become active.
    class WindowBinding
    {
        std::unordered_map<HWND, WNDPROC> originals_;
        HWND active_ = nullptr;
    public:
        HWND Active() const { return active_; }
        WNDPROC Original(HWND window) const
        {
            const auto it = originals_.find(window);
            return it == originals_.end() ? nullptr : it->second;
        }
        bool Bind(HWND window, WNDPROC hook)
        {
            DWORD pid = 0;
            if (!window || !hook || !IsWindow(window) ||
                !GetWindowThreadProcessId(window, &pid) || pid != GetCurrentProcessId()) return false;
            if (Original(window)) { active_ = window; return true; }
            const auto original = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(window, GWLP_WNDPROC));
            if (!original || original == hook) return false;
            // Allocate before installing the hook so allocation failure cannot
            // leave a hooked window without its forwarding target.
            originals_.emplace(window, original);
            SetLastError(0);
            const auto previous = SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hook));
            if (!previous)
            {
                originals_.erase(window);
                return false;
            }
            originals_[window] = reinterpret_cast<WNDPROC>(previous);
            active_ = window;
            return true;
        }
        void Forget(HWND window)
        {
            originals_.erase(window);
            if (active_ == window) active_ = nullptr;
        }
    };
}
