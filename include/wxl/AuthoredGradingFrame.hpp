// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AuthoredGradingApi.h"
#include <cmath>
#include <algorithm>
#include <limits>
namespace wxl::grading {
inline bool ValidCurve(const WXL_AuthoredGrading& v) {
    if(v.structSize!=sizeof(v) || !std::isfinite(v.strength) || v.strength<=0 || v.strength>1) return false;
    for(float f:v.curve) if(!std::isfinite(f) || f<0 || f>1) return false;
    return true;
}
class AuthoredFrame {
    WXL_AuthoredGrading value{};
    uint64_t frame=0,generation=0;
public:
    void Reset() { value={}; frame=generation=0; }
    bool Set(uint64_t f,uint64_t g,const WXL_AuthoredGrading& v) {
        Reset(); if(!f || !g) return false;
        auto normalized=v;
        // Convex light/time blends can overshoot an endpoint by a few ULPs.
        // Normalize only rounding-sized error at this producer boundary; keep
        // the consumer contract strict and reject genuinely invalid input.
        constexpr float tolerance=8*std::numeric_limits<float>::epsilon();
        for(float& sample:normalized.curve) {
            if(!std::isfinite(sample) || sample < -tolerance || sample > 1.f+tolerance) return false;
            sample=std::clamp(sample,0.f,1.f);
        }
        if(!ValidCurve(normalized)) return false;
        frame=f; generation=g; value=normalized; return true;
    }
    int Get(uint64_t f,uint64_t g,WXL_AuthoredGrading* out) const {
        if(!out || out->structSize!=sizeof(*out)) return 0;
        *out={sizeof(*out)};
        if(f!=frame || g!=generation || !ValidCurve(value)) return 0;
        *out=value; return 1;
    }
};
}
