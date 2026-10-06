// Core-owned opt-in transparent fog integration and passive compatibility probe.
// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TransparentFogLayout.hpp"
#include "TransparentFogBridge.hpp"
#include <cmath>
#include <intrin.h>
#include "wxl/TransparentFogApi.h"
#include "wxl/RenderDefaults.hpp"
#include "runtime/Extensions.hpp"
#include "engine/render/RenderService.hpp"
#include "game/Gx.hpp"
namespace wxl::render::transparent {
struct Args { float start,end,exponent; const uint32_t* colour; };
struct Probe {
 bool enabled=false,world=false,pastLiquid=false;
 uint64_t frames=0,before=0,after=0,outside=0,liquids=0,invalid=0,lighting=0,additive=0,modulate=0,other=0;
 ULONGLONG started=0,next=0; unsigned examples=0;
 void Begin() { if(!enabled)return;world=true;pastLiquid=false;++frames;if(!started)started=GetTickCount64(); }
 void End() {
  world=false;pastLiquid=false;
  if(!enabled) return;
  const auto now=GetTickCount64();
  if(now>=next) { next=now+10000;
   WLOG_INFO("transparent-probe: frames=%llu liquid-boundaries=%llu M2-before=%llu after=%llu outside=%llu invalid=%llu lighting=%llu additive=%llu modulate=%llu other=%llu",frames,liquids,before,after,outside,invalid,lighting,additive,modulate,other);
  }
  if(now-started>120000) enabled=false;
 }
};
inline Probe probe;
inline bool integration=false,inWorld=false,atBoundary=false,glareDrawn=false;
inline void(__cdecl* observer)(WXL_M2FogArgs*)=nullptr;
inline void(__cdecl* nativeGlare)()=nullptr;
inline int __cdecl SetObserver(void(__cdecl* fn)(WXL_M2FogArgs*)) { if(observer && observer!=fn)return 0;observer=fn;return 1; }
inline int __cdecl DrawGlare() { if(!integration || !inWorld || !atBoundary || glareDrawn)return 0;glareDrawn=true;nativeGlare();return 1; }
inline void __cdecl GlareBridge() { if(!inWorld || !glareDrawn || reinterpret_cast<uintptr_t>(_ReturnAddress())!=0x004F9218)nativeGlare(); }
inline WXL_TransparentFogApi api{sizeof(api),1,SetObserver,DrawGlare};
inline void Begin() { inWorld=true;glareDrawn=false;atBoundary=false;probe.Begin(); }
inline void Cancel() { inWorld=false;atBoundary=false;glareDrawn=false; }
inline void End() { inWorld=false;atBoundary=false;glareDrawn=false;probe.End(); }

inline void(__cdecl* nativeFog)()=nullptr;
inline void(__cdecl* nativeLiquid)()=nullptr;
inline void __cdecl Observe(uintptr_t caller,const Args* args) noexcept {
 if(caller!=0x0081FD1A) return;
 if(integration && inWorld && observer) observer(reinterpret_cast<WXL_M2FogArgs*>(const_cast<Args*>(args)));
 if(!probe.enabled) return;
 __try {
  if(!probe.world) {++probe.outside;return;}
  if(probe.pastLiquid)++probe.after;else ++probe.before;
  if(!args || !args->colour || !std::isfinite(args->start) || !std::isfinite(args->end) || !std::isfinite(args->exponent)) {++probe.invalid;return;}
  const auto colour=*args->colour;
  if((colour&0xFF000000)==0xFF000000)++probe.lighting;
  else if(colour==0)++probe.additive;
  else if(colour==0xFFFFFF || colour==0x808080)++probe.modulate;
  else ++probe.other;
  if(probe.pastLiquid && probe.examples<6) {++probe.examples;WLOG_INFO("transparent-probe: after-liquid fog start=%.6g end=%.6g exponent=%.6g colour=%08X",args->start,args->end,args->exponent,colour);}
 } __except(1) {++probe.invalid;probe.enabled=false;WLOG_WARN("transparent-probe: read exception; observation disabled");}
}
inline void __cdecl LiquidEnd(uintptr_t caller) noexcept {
 if(integration && inWorld && caller==0x004F9175) {
  atBoundary=true;wxl::render::BeforeTransparents(wxl::game::gx::RawDevice());atBoundary=false;
 }
 if(probe.enabled && probe.world && caller==0x004F9175) {probe.pastLiquid=true;++probe.liquids;}
}
WXL_FOG_ARGUMENT_BRIDGE(FogBridge,Observe,nativeFog)
WXL_LIQUID_END_BRIDGE(LiquidBridge,LiquidEnd,nativeLiquid)
inline void InstallProbe() {
 wchar_t value[8]{}; const bool capture=GetEnvironmentVariableW(L"WXL_TRANSPARENT_FOG_PROBE",value,8) && value[0]==L'1';
 // Accepted path is on for normal launches; passive probes stay observation-only.
 const bool requested=!capture && wxl::render::defaults::Enabled("WXL_TRANSPARENT_FOG");
 if(!capture && !requested)return;
 if(!Layout()) {WLOG_WARN("transparent-probe: client layout mismatch; no hooks installed");return;}
 const bool fog=wxl::hook::Install("TransparentFogProbe",0x00873210,&FogBridge,&nativeFog);
 const bool liquid=wxl::hook::Install("TransparentLiquidProbe",0x0077F020,&LiquidBridge,&nativeLiquid);
 probe.enabled=capture && fog && liquid;
 integration=requested && fog && liquid && wxl::hook::Install("TransparentGlare",0x007F0870,&GlareBridge,&nativeGlare);
 if(integration) wxl::runtime::extensions::PublishInterface("wxl.transparent-fog",1,&api);
 WLOG_INFO("transparent-fog: integration hooks=%u",integration);
 if(capture) WLOG_INFO("transparent-probe: layout matched; passive observation=%u integration=%u",probe.enabled,integration);
}
}
