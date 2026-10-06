// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
namespace wxl::render
{
    inline bool ValidDepthCamera(const float* view, const float* projection, const float* position)
    {
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(view[i]) || !std::isfinite(projection[i])) return false;
        for (int i = 0; i < 3; ++i) if (!std::isfinite(position[i])) return false;
        const float det = view[0]*(view[5]*view[10]-view[6]*view[9]) -
            view[1]*(view[4]*view[10]-view[6]*view[8]) + view[2]*(view[4]*view[9]-view[5]*view[8]);
        if (std::abs(det) < 0.00001f || projection[0] <= 0 || projection[5] <= 0 ||
            std::abs(projection[11] - 1.0f) > 0.00001f || std::abs(projection[15]) > 0.00001f ||
            projection[10] <= 1.0f || projection[14] >= 0.0f) return false;
        const float nearPlane = -projection[14] / projection[10];
        const float farPlane = -projection[14] / (projection[10] - 1.0f);
        return nearPlane > 0 && std::isfinite(farPlane) && farPlane > nearPlane;
    }
}
