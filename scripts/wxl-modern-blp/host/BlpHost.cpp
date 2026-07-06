// Host face for wxl-modern-blp: serves an uncompressed-BGRA texture as a DXT5 texture the Client reads.
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

#include "Host.hpp"

#include "core/Logger.hpp"

#include "../shared/BlpTranscode.hpp"

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Fires on every .blp open. A texture in the uncompressed-BGRA encoding (3) the Client cannot decode is
// re-encoded to DXT5; every other texture is served raw (the transcode declines fast after the header).
namespace
{
    bool EndsWithCI(std::string_view s, std::string_view suffix)
    {
        if (suffix.size() > s.size()) return false;
        const size_t off = s.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
                std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
        return true;
    }

    bool StartsWithCI(std::string_view s, std::string_view prefix)
    {
        if (prefix.size() > s.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
        return true;
    }

    bool IsTextureComponent(std::string_view name)
    {
        return StartsWithCI(name, "item\\texturecomponents\\")
            || StartsWithCI(name, "item/texturecomponents/");
    }

    int FindConfigInt(const std::string& text, const char* key, int fallback)
    {
        const std::string needle = std::string("SET ") + key;
        size_t pos = text.find(needle);
        if (pos == std::string::npos) return fallback;
        pos += needle.size();
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        if (pos < text.size() && text[pos] == '"') ++pos;
        bool neg = false;
        if (pos < text.size() && text[pos] == '-') { neg = true; ++pos; }
        int value = 0;
        bool any = false;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
        {
            any = true;
            value = value * 10 + int(text[pos] - '0');
            ++pos;
        }
        if (!any) return fallback;
        return neg ? -value : value;
    }

    std::string ReadTextFile(const std::string& path)
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return {};
        std::string text;
        char buf[4096];
        for (;;)
        {
            const size_t n = std::fread(buf, 1, sizeof(buf), f);
            if (n) text.append(buf, n);
            if (n < sizeof(buf)) break;
        }
        std::fclose(f);
        return text;
    }

    uint32_t ComponentTextureMaxEdge()
    {
        static bool loaded = false;
        static uint32_t maxEdge = 256;
        if (loaded) return maxEdge;
        loaded = true;

        const std::string root = wxl::host::ClientRoot();
        const std::string cfg = root.empty()
            ? ReadTextFile("WTF\\Config.wtf")
            : ReadTextFile(root + "\\WTF\\Config.wtf");
        const int componentLevel = FindConfigInt(cfg, "componentTextureLevel", 0);

        if (componentLevel >= 9) maxEdge = 1024;
        else if (componentLevel >= 8) maxEdge = 512;
        else maxEdge = 256;

        wxl::core::log::Printf("modern-blp: componentTextureLevel=%d componentMaxEdge=%u",
                               componentLevel, maxEdge);
        return maxEdge;
    }

    // Larger edge a served terrain/model texture is capped to. Oversized modern art (2048/4096) is the
    // main consumer of the client's tight 32-bit address space; serving its existing 1024 mip cuts that
    // ~4x with no decode and no client change.
    constexpr uint32_t kMaxTextureEdge = 1024;

    bool TransformBlp(std::string_view name, std::span<const uint8_t> raw, std::vector<uint8_t>& out)
    {
        if (!EndsWithCI(name, ".blp")) return false;

        const uint32_t componentMaxEdge = ComponentTextureMaxEdge();
        if (IsTextureComponent(name) && wxl::modern::blp::TextureComponentToPaletted(raw, out, componentMaxEdge))
        {
            wxl::core::log::Printf("modern-blp: %.*s DXT component->paletted maxEdge=%u (%u -> %u bytes)",
                int(name.size()), name.data(), componentMaxEdge, uint32_t(raw.size()), uint32_t(out.size()));
            return true;
        }

        // Cap oversized textures first (drops the top mip level), then transcode if the encoding needs it.
        std::vector<uint8_t> capped;
        std::span<const uint8_t> src = raw;
        const bool didCap = wxl::modern::blp::CapBlpMips(raw, capped, kMaxTextureEdge);
        if (didCap) src = capped;

        std::vector<uint8_t> transcoded;
        if (wxl::modern::blp::TranscodeBlp(src, transcoded))
        {
            out = std::move(transcoded);
            wxl::core::log::Printf("modern-blp: %.*s %sBGRA->DXT5 (%u -> %u bytes)",
                int(name.size()), name.data(), didCap ? "capped+" : "",
                uint32_t(raw.size()), uint32_t(out.size()));
            return true;
        }
        if (didCap)
        {
            out = std::move(capped);
            wxl::core::log::Printf("modern-blp: %.*s capped to %u (%u -> %u bytes)",
                int(name.size()), name.data(), kMaxTextureEdge, uint32_t(raw.size()), uint32_t(out.size()));
            return true;
        }
        return false;
    }

    struct Registrar
    {
        Registrar() { wxl::host::RegisterTransform("modern-blp", &TransformBlp); }
    } g_registrar;
}
