#include "EnvironmentModule.hpp"
#include "extension-support/environment/UiApi.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/ui/ImGuiHost.hpp"
#include "runtime/Extensions.hpp"
#include "wxl/EnvironmentWindApi.hpp"
#include "imgui.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
const WXL_Api* g_api = nullptr;
const WXL_EnvironmentUiApi* g_ui = nullptr;
struct Entry { const char* name; bool enabled; bool (*install)(); wxl::hook::Phase phase; };
std::vector<Entry>& Entries() { static std::vector<Entry> entries; return entries; }
bool g_loaded = false;
}

namespace wxl::hook {
void RegisterFeature(const char* name, bool enabled, bool (*install)(), Phase phase) {
    Entries().push_back({name, enabled, install, phase});
}
bool Install(const char* name, void* target, void* detour, void** original, int priority) {
    return g_api && g_api->HookAttach(name, reinterpret_cast<uintptr_t>(target), detour, original, priority) != 0;
}
}
namespace wxl::events {
void Subscribe(Event event, Handler handler, void* user) { g_api->Subscribe(static_cast<uint32_t>(event), handler, user); }
void Emit(Event event, const void* args) { g_api->Emit(static_cast<uint32_t>(event), args); }
}
namespace wxl::log {
bool Enabled(Level) { return g_api != nullptr; }
void WriteV(Level level, const char* fmt, va_list args) {
    char buffer[4096]; vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (g_api) g_api->Log(static_cast<int>(level), WXL_ENVIRONMENT_NAME, "%s", buffer);
}
void Write(Level level, const char* fmt, ...) { va_list args; va_start(args, fmt); WriteV(level, fmt, args); va_end(args); }
}
namespace wxl::runtime::extensions {
void PublishInterface(const char* name, uint32_t version, void* iface) { if(g_api) g_api->PublishInterface(name,version,iface); }
void* GetInterface(const char* name, uint32_t version) { return g_api ? g_api->GetInterface(name, version) : nullptr; }
}
namespace wxl::ui {
void AddPanel(const char* title, PanelFn fn, void* user, float width, float height) { g_ui->AddPanel(title, fn, user, width, height); }
void AddWindowOwner(const char* name, PanelFn fn, void* user) { g_ui->AddWindowOwner(name, fn, user); }
bool IsOpen() { return g_api->UiIsOpen() != 0; }
}

#ifdef WXL_ENVIRONMENT_WIND_CLIENT
namespace {
const WXL_EnvironmentWindApi* WindApi() {
    // Do not cache a miss: discovery order must not decide whether water sees wind.
    static const WXL_EnvironmentWindApi* cached = nullptr;
    if (!cached && g_api) {
        auto* api = static_cast<const WXL_EnvironmentWindApi*>(g_api->GetInterface("wxl.environment-wind", 1));
        if (api && api->structSize == sizeof(*api) && api->profileSize == sizeof(wxl::wind::WindProfile) &&
            api->sampleSize == sizeof(wxl::wind::WindSample) && api->Settings && api->Frame) cached = api;
    }
    return cached;
}
}
namespace wxl::wind {
WindProfile& Settings() { static WindProfile fallback; auto* api = WindApi(); return api ? *api->Settings() : fallback; }
const WindSample& Frame() { static const WindSample calm; auto* api = WindApi(); return api ? *api->Frame() : calm; }
}
#endif

const WXL_PluginInfo* __cdecl WXL_Query() {
    static const WXL_PluginInfo info = {sizeof(WXL_PluginInfo), WXL_API_VERSION, WXL_ENVIRONMENT_NAME, 1, WXL_CLIENT_BUILD};
    return &info;
}
int __cdecl WXL_Load(const WXL_Api* api) {
    if (!api || api->apiVersion != WXL_API_VERSION || api->structSize < sizeof(WXL_Api)) return 0;
    if (g_loaded) return 1;
    g_api = api;
    char value[2]{};
    const bool enabled = !(GetEnvironmentVariableA(WXL_ENVIRONMENT_SWITCH, value, sizeof(value)) == 1 && value[0] == '0');
    if (!enabled) { api->Log(WXL_LOG_INFO, WXL_ENVIRONMENT_NAME, "environment-module-v1: disabled by %s", WXL_ENVIRONMENT_SWITCH); g_loaded = true; return 1; }
    g_ui = static_cast<const WXL_EnvironmentUiApi*>(api->GetInterface("wxl.environment-ui", 1));
    if (!g_ui || g_ui->structSize != sizeof(*g_ui) || !g_ui->imguiVersion ||
        std::strcmp(g_ui->imguiVersion, IMGUI_VERSION) != 0 ||
        g_ui->ioSize != sizeof(ImGuiIO) || g_ui->styleSize != sizeof(ImGuiStyle) ||
        g_ui->vec2Size != sizeof(ImVec2) || g_ui->vec4Size != sizeof(ImVec4) || !g_ui->AddPanel || !g_ui->AddWindowOwner) {
        api->Log(WXL_LOG_ERROR, WXL_ENVIRONMENT_NAME, "environment UI ABI mismatch; install a matched core and environment module build"); return 0;
    }
    unsigned installed = 0;
    for (const auto& entry : Entries()) {
        if (!entry.enabled) continue;
        // All migrated features are Normal. Refuse new phases until the loader
        // supports their lifecycle instead of silently installing them too early.
        if (entry.phase != wxl::hook::Phase::Normal || !entry.install || !entry.install()) {
            api->Log(WXL_LOG_ERROR, WXL_ENVIRONMENT_NAME, "feature '%s' failed", entry.name); return 0;
        }
        ++installed;
    }
#ifdef WXL_ENVIRONMENT_WIND_PROVIDER
    static WXL_EnvironmentWindApi wind = {sizeof(WXL_EnvironmentWindApi), sizeof(wxl::wind::WindProfile), sizeof(wxl::wind::WindSample),
        []() -> wxl::wind::WindProfile* { return &wxl::wind::Settings(); },
        []() -> const wxl::wind::WindSample* { return &wxl::wind::Frame(); }};
    api->PublishInterface("wxl.environment-wind", 1, &wind);
#endif
    api->Log(WXL_LOG_INFO, WXL_ENVIRONMENT_NAME, "environment-module-v1: installed %u features", installed);
    g_loaded = true;
    return 1;
}
