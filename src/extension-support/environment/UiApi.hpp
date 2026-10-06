#pragma once
#include <cstdint>
// Private, matched-build ABI for the first-party environment DLLs. These import
// ImGui functions from the core; no second ImGui context or allocator is created.
// Third-party extensions continue to use the stable WXL_Api::Ui* controls.
struct WXL_EnvironmentUiApi {
    uint32_t structSize;
    const char* imguiVersion;
    uint32_t ioSize, styleSize, vec2Size, vec4Size;
    void (__cdecl* AddPanel)(const char*, void (__cdecl*)(void*), void*, float, float);
    void (__cdecl* AddWindowOwner)(const char*, void (__cdecl*)(void*), void*);
};
namespace wxl::ui { const WXL_EnvironmentUiApi* EnvironmentApi(); }
