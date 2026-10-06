// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace wxl::render
{
    // D3D row-vector projection: ndcZ = p[10] + p[14] / viewZ.
    inline constexpr char kDepthPreviewShader[] =
        "sampler2D depthTex:register(s0); float4 projection:register(c0);"
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {"
        "float rawZ=tex2D(depthTex,uv).r;"
        "float z=(rawZ-projection.z)*projection.w;"
        "float distance=projection.y/min(z-projection.x,-0.000001);"
        "float c=saturate(distance/100.0);return float4(c,c,c,1);}";
}
