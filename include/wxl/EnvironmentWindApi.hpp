#pragma once
#include "wxl/EnvironmentWind.hpp"
// Version 1 exchanges process-lifetime POD storage only, never owning containers
// or allocations. Both sides validate the structure sizes before dereferencing.
struct WXL_EnvironmentWindApi {
    uint32_t structSize, profileSize, sampleSize;
    wxl::wind::WindProfile* (__cdecl* Settings)();
    const wxl::wind::WindSample* (__cdecl* Frame)();
};
