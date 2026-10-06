// In-game ImGui host: device lifetime, input routing, and a registry of panels to draw.
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
//
// MECHANISM
//   Everything this needs already exists as an event, so nothing here hooks anything: the overlay
//   builds and draws on OnEndScene (the frame is complete, the device is live, and drawing here
//   lands on top of the game's own UI), releases its device objects on OnDeviceLost and rebuilds
//   them on OnDeviceReset, and reads the keyboard and mouse through OnInput, which is swallowable.
//
//   THE ONE RULE WORTH KNOWING: the overlay only consumes input while it is OPEN. A debug overlay
//   that eats keystrokes when it is not visible is indistinguishable from a broken game, and the
//   report that comes back is never "your overlay is stealing input".
//
//   Initialisation is lazy and happens on the first OnEndScene, because the device does not exist
//   when features install. Everything degrades to "no overlay" rather than to a crash.

#include "config.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "engine/ui/ImGuiHost.hpp"
#include "game/Gx.hpp"

#include "common/Log.hpp"

#include <windows.h>
#include <d3d9.h>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <cstring>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace
{
    namespace ev = wxl::events;

    struct Panel
    {
        const char*      title;
        wxl::ui::PanelFn fn;
        void*            user;
        float            firstWidth;
        float            firstHeight;
        bool             ownsWindows;
        bool             visible = false;
    };

    constexpr int kMaxPanels = 24;
    Panel g_panels[kMaxPanels]{};
    int   g_panelCount = 0;

    bool  g_ready   = false;   // backends initialised
    bool  g_failed  = false;   // initialisation failed once; do not retry every frame
    bool  g_open    = false;   // overlay visible and taking input
    HWND  g_hwnd    = nullptr;
    IDirect3DDevice9* g_device = nullptr; // borrowed; the initialized DX9 backend retains it

    /// The general tools launcher. Depth diagnostics use F8 separately.
    constexpr int kToggleKey = VK_F9;

    HWND WindowOfDevice(IDirect3DDevice9* dev)
    {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        if (SUCCEEDED(dev->GetCreationParameters(&cp)) && cp.hFocusWindow) return cp.hFocusWindow;
        return GetActiveWindow();
    }

    bool EnsureReady(IDirect3DDevice9* dev)
    {
        if (!dev) return false;
        const HWND window = WindowOfDevice(dev);
        if (g_ready && g_device == dev && g_hwnd == window) return true;
        // Graphics settings can replace both device and window without Reset.
        // Keep the ImGui context (layout/panel state), but retire both old backends.
        if (g_ready)
        {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            g_ready = false;
            WLOG_INFO("imgui: rebinding replaced graphics device/window");
        }
        if (g_device != dev || g_hwnd != window) g_failed = false;
        if (g_failed) return false;
        g_device = dev;

        g_hwnd = window;
        if (!g_hwnd) { g_failed = true; WLOG_WARN("imgui: no window, overlay disabled"); return false; }

        IMGUI_CHECKVERSION();
        if (!ImGui::GetCurrentContext())
        {
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::StyleColorsDark();
        }

        const bool windowReady = ImGui_ImplWin32_Init(g_hwnd);
        if (!windowReady || !ImGui_ImplDX9_Init(dev))
        {
            if (windowReady) ImGui_ImplWin32_Shutdown();
            g_failed = true;
            WLOG_WARN("imgui: backend init failed, overlay disabled");
            return false;
        }
        g_ready = true;
        WLOG_INFO("imgui: overlay ready (hwnd=%p) -- F9 toggles", g_hwnd);
        return true;
    }

    void OnEndScene(void*, const void* args)
    {
        auto* a = static_cast<const ev::EndSceneArgs*>(args);
        auto* dev = static_cast<IDirect3DDevice9*>(a->device);
        if (!EnsureReady(dev)) return;
        if (!g_open) return;

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Opening tools should not open every registered module's windows. Keep each workspace
        // opt-in, including owners such as Water that manage several windows themselves.
        ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("WarcraftXL tools", &g_open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("F9: tools | F8: depth preview (when enabled)");
            ImGui::TextUnformatted("F11: enhanced/native renderer (when enabled)");
            ImGui::TextUnformatted("F12: reload saved water, atmosphere, shadows and grading");
            ImGui::Separator();
            for (int i = 0; i < g_panelCount; ++i)
            {
                ImGui::PushID(i);
                ImGui::Checkbox(g_panels[i].title, &g_panels[i].visible);
                ImGui::PopID();
            }
        }
        ImGui::End();

        for (int i = 0; g_open && i < g_panelCount; ++i)
        {
            Panel& panel = g_panels[i];
            if (!panel.visible) continue;
            if (panel.ownsWindows)
            {
                panel.fn(panel.user);
                continue;
            }

            if (panel.firstWidth > 0.0f || panel.firstHeight > 0.0f)
                ImGui::SetNextWindowSize(ImVec2(panel.firstWidth, panel.firstHeight),
                                         ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(340.0f, 16.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(panel.title, &panel.visible)) panel.fn(panel.user);
            ImGui::End();
        }

        ImGui::EndFrame();
        ImGui::Render();

        // The game leaves state set for whatever it drew last, and the DX9 backend assumes nothing.
        // A state block is the cheap way to be certain the overlay hands the device back untouched;
        // without it the first frame after the overlay opens can lose the game's own render states.
        IDirect3DStateBlock9* block = nullptr;
        if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &block)) && block)
        {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            block->Apply();
            block->Release();
        }
        else
        {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        }
    }

    void OnDeviceLost(void*, const void*)
    {
        if (g_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
    }

    void OnDeviceReset(void*, const void* args)
    {
        const auto* event = static_cast<const ev::DeviceResetArgs*>(args);
        if (event && EnsureReady(static_cast<IDirect3DDevice9*>(event->device)))
            ImGui_ImplDX9_CreateDeviceObjects();
    }

    void OnInput(void*, const void* args)
    {
        auto* a = const_cast<ev::InputArgs*>(static_cast<const ev::InputArgs*>(args));

        // The toggle is read before anything else and never forwarded, so the key cannot also reach
        // the game. Handled on key-UP: a key-DOWN repeats while held and the overlay would strobe.
        if ((a->message == WM_KEYDOWN || a->message == WM_KEYUP) &&
            static_cast<int>(a->wparam) == kToggleKey)
        {
            if (g_ready && a->message == WM_KEYUP)
            {
                g_open = !g_open;
                // The client hides and clips the cursor for mouselook. Releasing the clip is what
                // makes the overlay actually usable; the client re-establishes it on its own once
                // the player moves the camera again, so nothing has to be restored here.
                if (g_open) ClipCursor(nullptr);
            }
            *a->handled = true;
            return;
        }

        if (!g_ready || !g_open) return;

        ImGui_ImplWin32_WndProcHandler(g_hwnd, a->message, static_cast<WPARAM>(a->wparam),
                                       static_cast<LPARAM>(a->lparam));

        // Swallow only what ImGui actually wants. Letting mouse messages through while a slider is
        // being dragged makes the camera spin under the panel; swallowing everything makes the game
        // unplayable with the overlay merely open.
        const ImGuiIO& io = ImGui::GetIO();
        const bool mouse = a->message >= WM_MOUSEFIRST && a->message <= WM_MOUSELAST;
        const bool keyb  = (a->message >= WM_KEYFIRST && a->message <= WM_KEYLAST);
        if ((mouse && io.WantCaptureMouse) || (keyb && io.WantCaptureKeyboard)) *a->handled = true;
    }

    bool InstallImGuiHost()
    {
        ev::Subscribe(ev::Event::OnEndScene,    &OnEndScene,    nullptr);
        ev::Subscribe(ev::Event::OnDeviceLost,  &OnDeviceLost,  nullptr);
        ev::Subscribe(ev::Event::OnDeviceReset, &OnDeviceReset, nullptr);
        ev::Subscribe(ev::Event::OnInput,       &OnInput,       nullptr);
        return true;
    }
}

namespace wxl::ui
{
    void AddPanel(const char* title, PanelFn fn, void* user, float firstWidth, float firstHeight)
    {
        if (g_panelCount >= kMaxPanels || !title || !fn) return;
        g_panels[g_panelCount++] = Panel{ title, fn, user, firstWidth, firstHeight, false };
    }

    void AddWindowOwner(const char* name, PanelFn fn, void* user)
    {
        if (g_panelCount >= kMaxPanels || !name || !fn) return;
        g_panels[g_panelCount++] = Panel{ name, fn, user, 0.0f, 0.0f, true };
    }

    bool IsOpen() { return g_open; }

    // A panel body runs between NewFrame and Render and inside an open window, so these need no
    // guard of their own beyond the null checks: the host only ever calls a body from there.
    namespace c
    {
        void __cdecl AddPanel(const char* title, void(__cdecl* fn)(void*), void* user)
        { wxl::ui::AddPanel(title, fn, user); }

        int __cdecl IsOpen() { return g_open ? 1 : 0; }

        void __cdecl Text(const char* text)
        { if (text) ImGui::TextUnformatted(text); }

        void __cdecl Separator() { ImGui::Separator(); }

        int __cdecl Button(const char* label)
        { return (label && ImGui::Button(label)) ? 1 : 0; }

        int __cdecl Checkbox(const char* label, int* value)
        {
            if (!label || !value) return 0;
            bool on = (*value != 0);
            const bool changed = ImGui::Checkbox(label, &on);
            if (changed) *value = on ? 1 : 0;
            return changed ? 1 : 0;
        }

        int __cdecl SliderFloat(const char* label, float* value, float min, float max)
        {
            if (!label || !value) return 0;
            return ImGui::SliderFloat(label, value, min, max) ? 1 : 0;
        }

        int __cdecl SliderInt(const char* label, int* value, int min, int max)
        {
            if (!label || !value) return 0;
            return ImGui::SliderInt(label, value, min, max) ? 1 : 0;
        }

        int __cdecl ColorEdit(const char* label, float rgba[4])
        {
            if (!label || !rgba) return 0;
            return ImGui::ColorEdit4(label, rgba) ? 1 : 0;
        }

        void __cdecl SameLine() { ImGui::SameLine(); }

        int __cdecl Combo(const char* label, int* index, const char* const* items, int count)
        {
            if (!label || !index || !items || count <= 0) return 0;
            // A caller's selection routinely outlives the list it was picked from -- the panel that
            // wants this walks one subject after another -- so an index outside the current list is
            // ordinary input and is brought back in range rather than refused.
            if (*index < 0) *index = 0;
            else if (*index >= count) *index = count - 1;
            return ImGui::Combo(label, index, items, count) ? 1 : 0;
        }

        int __cdecl CollapsingHeader(const char* label)
        { return (label && ImGui::CollapsingHeader(label)) ? 1 : 0; }

        int __cdecl InputText(const char* label, char* buf, size_t bufSize)
        {
            if (!label || !buf || bufSize == 0) return 0;
            return ImGui::InputText(label, buf, bufSize) ? 1 : 0;
        }
    }
}

WXL_REGISTER_FEATURE("imgui-host", wxl::features::imguiOverlay, InstallImGuiHost)
