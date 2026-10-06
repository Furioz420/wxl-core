// Core-owned native boundary. Borrowed arguments; callback must not retain them.
#pragma once
#include <stdint.h>
struct WXL_M2FogArgs { float start,end,exponent; const uint32_t* colour; };
struct WXL_TransparentFogApi {
 uint32_t structSize,apiVersion;
 int(__cdecl* SetObserver)(void(__cdecl*)(WXL_M2FogArgs*));
 int(__cdecl* DrawGlare)(); // Only valid in the current pre-transparent callback.
};
