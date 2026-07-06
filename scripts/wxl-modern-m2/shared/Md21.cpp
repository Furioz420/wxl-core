// MD21 container: de-chunk a chunked modern M2 to a self-contained MD20 the Client loader reads.
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

#include "Md21.hpp"

#include "Contract.hpp"
#include "core/Logger.hpp"
#include "structure/m2/M2Format.hpp"

#include <limits>
#include <string>
#include <string_view>

namespace wxl::scripts::modernm2::md21
{
    namespace fmt = wxl::structure::m2;

    namespace
    {
        // Auxiliary chunk magics are stored in memory order (not reversed), so a plain little-endian read
        // matches these.
        constexpr uint32_t Magic(char a, char b, char c, char d)
        {
            return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
                   (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
        }
        constexpr uint32_t kTXID = Magic('T', 'X', 'I', 'D');
        constexpr uint32_t kSKID = Magic('S', 'K', 'I', 'D');
        constexpr uint32_t kSFID = Magic('S', 'F', 'I', 'D');
        constexpr uint32_t kAFID = Magic('A', 'F', 'I', 'D');
        constexpr uint32_t kBFID = Magic('B', 'F', 'I', 'D');
        constexpr uint32_t kSKS1 = Magic('S', 'K', 'S', '1');
        constexpr uint32_t kSKB1 = Magic('S', 'K', 'B', '1');

        // M2 header field offsets the de-chunk touches.
        constexpr uint32_t kHdrSequences  = 0x1C; // M2Array: animation sequence records
        constexpr uint32_t kHdrTextures   = 0x50; // M2Array: texture records
        constexpr uint32_t kHdrBoneCombos = 0x78; // M2Array: bone lookup table
        constexpr uint32_t kHdrBones      = 0x2C; // M2Array: bone records
        constexpr uint32_t kHdrBoneLookup = 0x34; // M2Array: key-bone lookup table
        // M2Texture record (0x10): type@0x00, flags@0x04, filename {count@0x08, offset@0x0C}.
        constexpr uint32_t kTexStride     = 0x10;
        constexpr uint32_t kTexNameCount  = 0x08;
        constexpr uint32_t kTexNameOffset = 0x0C;
        // M2CompBone record (0x58): keyBoneId@0x00, flags@0x04, parentBone@0x08, ... A bone whose flags
        // carry a billboard bit faces the camera; collapsing the lookup must not route geometry onto it.
        constexpr uint32_t kBoneStride        = 0x58;
        constexpr uint32_t kBoneFlags         = 0x04;
        constexpr uint32_t kBoneBillboardMask = 0x78; // spherical (0x8) + cylindrical-lock (0x10/0x20/0x40)
        constexpr uint32_t kSequenceStride    = sizeof(fmt::M2Sequence);

        struct ChunkView
        {
            const uint8_t* data = nullptr;
            uint32_t size = 0;
        };

        struct SkelInlineStats
        {
            uint32_t globalLoops = 0;
            uint32_t sequences = 0;
            uint32_t sequenceLookup = 0;
            uint32_t bones = 0;
            uint32_t boneLookup = 0;
            uint32_t skbBytes = 0;
        };

        bool FitsRange(uint32_t offset, uint32_t count, uint32_t stride, uint32_t size)
        {
            if (count == 0) return true;
            const uint64_t bytes = uint64_t(count) * uint64_t(stride);
            const uint64_t end = uint64_t(offset) + bytes;
            return end <= size && end >= offset;
        }

        bool AddOffset(uint32_t base, uint32_t offset, uint32_t& out)
        {
            const uint64_t v = uint64_t(base) + uint64_t(offset);
            if (v > std::numeric_limits<uint32_t>::max()) return false;
            out = static_cast<uint32_t>(v);
            return true;
        }

        bool AlignOut(std::vector<uint8_t>& out, uint32_t align)
        {
            if (align <= 1) return true;
            const size_t mask = size_t(align) - 1;
            while ((out.size() & mask) != 0)
            {
                if (out.size() >= std::numeric_limits<uint32_t>::max()) return false;
                out.push_back(0);
            }
            return true;
        }

        bool FindChunk(std::span<const uint8_t> bytes, uint32_t magic, ChunkView& out)
        {
            const uint8_t* b = bytes.data();
            const uint32_t n = static_cast<uint32_t>(bytes.size());
            for (uint32_t o = 0; o + 8 <= n; )
            {
                const uint32_t tag = Rd32(b + o), sz = Rd32(b + o + 4);
                if (o + 8 + uint64_t(sz) > n) return false;
                if (tag == magic)
                {
                    out.data = b + o + 8;
                    out.size = sz;
                    return true;
                }
                o += 8 + sz;
            }
            return false;
        }

        bool AppendChunkArray(std::vector<uint8_t>& out, const ChunkView& chunk, fmt::M2Array src,
                              uint32_t stride, uint32_t align, fmt::M2Array& dst)
        {
            dst = {};
            if (src.count == 0) return true;
            if (!FitsRange(src.offset, src.count, stride, chunk.size)) return false;
            if (!AlignOut(out, align)) return false;

            const uint64_t bytes = uint64_t(src.count) * uint64_t(stride);
            if (uint64_t(out.size()) + bytes > std::numeric_limits<uint32_t>::max()) return false;

            dst.count = src.count;
            dst.offset = static_cast<uint32_t>(out.size());
            out.insert(out.end(), chunk.data + src.offset, chunk.data + src.offset + bytes);
            return true;
        }

        fmt::M2Array ReadArray(const uint8_t* p)
        {
            return fmt::M2Array{ Rd32(p), Rd32(p + 4) };
        }

        bool EndsWithCI(std::string_view value, std::string_view suffix)
        {
            if (suffix.size() > value.size()) return false;
            const size_t start = value.size() - suffix.size();
            for (size_t i = 0; i < suffix.size(); ++i)
            {
                char a = value[start + i];
                char b = suffix[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                if (a != b) return false;
            }
            return true;
        }

        std::string InferSkeletonPath(std::string_view sourceName)
        {
            if (sourceName.empty()) return {};

            std::string path(sourceName);
            for (char& c : path)
                if (c == '/') c = '\\';

            if (EndsWithCI(path, ".m2")) path.resize(path.size() - 3);
            else if (EndsWithCI(path, ".mdx")) path.resize(path.size() - 4);
            else return {};
            path += ".skel";
            return path;
        }

        bool RebaseNestedArrayOffsets(uint8_t* skb, uint32_t skbSize, uint32_t modelBase,
                                      uint32_t arrayCount, uint32_t arrayOffset)
        {
            if (arrayCount == 0 || arrayOffset == 0) return true;
            if (!FitsRange(arrayOffset, arrayCount, sizeof(fmt::M2Array), skbSize)) return false;

            uint8_t* arrays = skb + arrayOffset;
            for (uint32_t i = 0; i < arrayCount; ++i)
            {
                uint8_t* rec = arrays + i * sizeof(fmt::M2Array);
                const uint32_t count = Rd32(rec);
                const uint32_t offset = Rd32(rec + 4);
                if (count == 0 || offset == 0 || offset == 0xFFFFFFFFu) continue;
                if (offset >= skbSize) return false;
                uint32_t rebased = 0;
                if (!AddOffset(modelBase, offset, rebased)) return false;
                Wr32(rec + 4, rebased);
            }
            return true;
        }

        bool RebaseTrack(uint8_t* skb, uint32_t skbSize, uint32_t modelBase, uint8_t* track)
        {
            // M2Track body: interp/globalSeq (u16/u16), timestamp M2Array, value M2Array.
            const uint32_t timeCount = Rd32(track + 0x04);
            const uint32_t timeOffset = Rd32(track + 0x08);
            const uint32_t valueCount = Rd32(track + 0x0C);
            const uint32_t valueOffset = Rd32(track + 0x10);

            if (!RebaseNestedArrayOffsets(skb, skbSize, modelBase, timeCount, timeOffset)) return false;
            if (!RebaseNestedArrayOffsets(skb, skbSize, modelBase, valueCount, valueOffset)) return false;

            if (timeCount && timeOffset && timeOffset != 0xFFFFFFFFu)
            {
                uint32_t rebased = 0;
                if (!AddOffset(modelBase, timeOffset, rebased)) return false;
                Wr32(track + 0x08, rebased);
            }
            if (valueCount && valueOffset && valueOffset != 0xFFFFFFFFu)
            {
                uint32_t rebased = 0;
                if (!AddOffset(modelBase, valueOffset, rebased)) return false;
                Wr32(track + 0x10, rebased);
            }
            return true;
        }

        bool InlineSks1(std::vector<uint8_t>& out, const ChunkView& sks, SkelInlineStats& stats)
        {
            if (sks.size < 0x18) return false;

            const fmt::M2Array srcLoops  = ReadArray(sks.data + 0x00);
            const fmt::M2Array srcSeqs   = ReadArray(sks.data + 0x08);
            const fmt::M2Array srcLookup = ReadArray(sks.data + 0x10);

            fmt::M2Array loops = {}, seqs = {}, lookup = {};
            if (!AppendChunkArray(out, sks, srcLoops,  4,               4, loops))  return false;
            if (!AppendChunkArray(out, sks, srcSeqs,   kSequenceStride, 4, seqs))   return false;
            if (!AppendChunkArray(out, sks, srcLookup, 2,               2, lookup)) return false;

            auto* md = reinterpret_cast<fmt::M2Header*>(out.data());
            if (loops.count)  md->globalLoops = loops;
            if (seqs.count)   md->sequences = seqs;
            if (lookup.count) md->sequenceLookup = lookup;

            stats.globalLoops = loops.count;
            stats.sequences = seqs.count;
            stats.sequenceLookup = lookup.count;
            return true;
        }

        bool InlineSkb1(std::vector<uint8_t>& out, const ChunkView& skb, SkelInlineStats& stats)
        {
            if (skb.size < 0x10) return false;

            const fmt::M2Array srcBones = ReadArray(skb.data + 0x00);
            const fmt::M2Array srcLookup = ReadArray(skb.data + 0x08);
            if (!FitsRange(srcBones.offset, srcBones.count, kBoneStride, skb.size)) return false;
            if (!FitsRange(srcLookup.offset, srcLookup.count, 2, skb.size)) return false;
            if (!AlignOut(out, 4)) return false;
            if (uint64_t(out.size()) + uint64_t(skb.size) > std::numeric_limits<uint32_t>::max()) return false;

            const uint32_t modelBase = static_cast<uint32_t>(out.size());
            out.insert(out.end(), skb.data, skb.data + skb.size);

            uint8_t* copiedSkb = out.data() + modelBase;
            uint8_t* bones = copiedSkb + srcBones.offset;
            for (uint32_t i = 0; i < srcBones.count; ++i)
            {
                uint8_t* bone = bones + i * kBoneStride;
                if (!RebaseTrack(copiedSkb, skb.size, modelBase, bone + 0x10)) return false;
                if (!RebaseTrack(copiedSkb, skb.size, modelBase, bone + 0x24)) return false;
                if (!RebaseTrack(copiedSkb, skb.size, modelBase, bone + 0x38)) return false;
            }

            uint32_t boneOffset = 0, lookupOffset = 0;
            if (!AddOffset(modelBase, srcBones.offset, boneOffset)) return false;
            if (!AddOffset(modelBase, srcLookup.offset, lookupOffset)) return false;

            auto* md = reinterpret_cast<fmt::M2Header*>(out.data());
            md->bones = fmt::M2Array{ srcBones.count, boneOffset };
            md->boneLookup = fmt::M2Array{ srcLookup.count, lookupOffset };

            stats.bones = srcBones.count;
            stats.boneLookup = srcLookup.count;
            stats.skbBytes = skb.size;
            return true;
        }

        bool InlineSkeleton(std::vector<uint8_t>& out, std::span<const uint8_t> skel, SkelInlineStats& stats)
        {
            if (out.size() < sizeof(fmt::M2Header)) return false;

            const auto* md = reinterpret_cast<const fmt::M2Header*>(out.data());
            const bool needSequences = md->globalLoops.count == 0 || md->sequences.count == 0 ||
                                       md->sequenceLookup.count == 0;
            const bool needBones = md->bones.count == 0 || md->boneLookup.count == 0;
            if (!needSequences && !needBones) return true;

            if (needSequences)
            {
                ChunkView sks;
                if (!FindChunk(skel, kSKS1, sks)) return false;
                if (!InlineSks1(out, sks, stats)) return false;
            }
            if (needBones)
            {
                ChunkView skb;
                if (!FindChunk(skel, kSKB1, skb)) return false;
                if (!InlineSkb1(out, skb, stats)) return false;
            }
            return true;
        }

        /**
         * @brief Reports whether any bone the lookup table references is a billboard bone.
         * @param md20  MD20 image base.
         * @param size  Image byte length.
         * @return True if at least one boneCombos entry resolves to a bone with a billboard flag set.
         */
        bool LookupReferencesBillboard(const uint8_t* md20, uint32_t size)
        {
            const uint32_t comboCount = Rd32(md20 + kHdrBoneCombos);
            const uint32_t comboOfs   = Rd32(md20 + kHdrBoneCombos + 4);
            const uint32_t boneCount  = Rd32(md20 + kHdrBones);
            const uint32_t boneOfs    = Rd32(md20 + kHdrBones + 4);
            for (uint32_t i = 0; i < comboCount; ++i)
            {
                if (comboOfs + i * 2 + 2 > size) break;
                const uint16_t bone = Rd16(md20 + comboOfs + i * 2);
                if (bone >= boneCount) continue;
                const uint32_t rec = boneOfs + bone * kBoneStride;
                if (rec + kBoneFlags + 4 > size) continue;
                if (Rd32(md20 + rec + kBoneFlags) & kBoneBillboardMask)
                    return true;
            }
            return false;
        }
    }

    bool IsMd21(std::span<const uint8_t> in)
    {
        return in.size() >= 8 && Rd32(in.data()) == fmt::kMagicMD21;
    }

    bool Dechunk(std::span<const uint8_t> in, TxidResolver resolve, std::vector<uint8_t>& out)
    {
        return Dechunk(in, resolve, nullptr, out);
    }

    bool Dechunk(std::span<const uint8_t> in, TxidResolver resolve, SidecarReader readSidecar,
                 std::vector<uint8_t>& out)
    {
        return Dechunk(in, {}, resolve, readSidecar, out);
    }

    bool Dechunk(std::span<const uint8_t> in, std::string_view sourceName, TxidResolver resolve,
                 SidecarReader readSidecar, std::vector<uint8_t>& out)
    {
        const uint8_t* b = in.data();
        const uint32_t n = static_cast<uint32_t>(in.size());
        if (n < 8 || Rd32(b) != fmt::kMagicMD21)
            return false;

        const uint32_t md20Size = Rd32(b + 4);
        if (8 + size_t(md20Size) > n || md20Size < sizeof(fmt::M2Header))
            return false;

        // Collect the known FileDataID sidecar chunks trailing the MD20 block. TXID is currently inlined
        // below. SKID/SFID/AFID/BFID are logged so split retail character models are obvious in host logs.
        std::vector<uint32_t> txid;
        std::vector<uint32_t> skid;
        uint32_t sfidCount = 0, afidCount = 0, bfidCount = 0;
        for (uint32_t o = 8 + md20Size; o + 8 <= n; )
        {
            const uint32_t tag = Rd32(b + o), sz = Rd32(b + o + 4);
            if (o + 8 + size_t(sz) > n) break;
            if (tag == kTXID)
                for (uint32_t k = 0; k + 4 <= sz; k += 4) txid.push_back(Rd32(b + o + 8 + k));
            else if (tag == kSKID)
                for (uint32_t k = 0; k + 4 <= sz; k += 4) skid.push_back(Rd32(b + o + 8 + k));
            else if (tag == kSFID) sfidCount = sz / 4;
            else if (tag == kAFID) afidCount = sz / 8; // pairs: anim id + FileDataID
            else if (tag == kBFID) bfidCount = sz / 4;
            o += 8 + sz;
        }

        // The Client reads MD20-relative offsets, so the extracted block can grow at its tail (the inlined
        // names) without disturbing any existing offset.
        out.assign(b + 8, b + 8 + md20Size);

        const uint32_t texCount = Rd32(out.data() + kHdrTextures);
        const uint32_t texOfs   = Rd32(out.data() + kHdrTextures + 4);
        uint32_t namedSlots = 0, weaponBladeSlots = 0, inlined = 0;
        for (uint32_t i = 0; i < texCount; ++i)
        {
            const uint32_t rec = texOfs + i * kTexStride;
            if (rec + kTexStride > out.size()) break;
            const uint32_t texType = Rd32(out.data() + rec);
            if (texType != fmt::kTexTypeHardcoded && texType != fmt::kTexTypeWeaponBlade)
                continue; // not a named or texture-aware replaceable slot
            if (Rd32(out.data() + rec + kTexNameCount) != 0) continue; // already has an inline name
            ++namedSlots;
            if (texType == fmt::kTexTypeWeaponBlade) ++weaponBladeSlots;

            std::string path;
            if (i >= txid.size() || !txid[i] || !resolve || !resolve(txid[i], path) || path.empty())
                continue;

            const uint32_t nameOfs = static_cast<uint32_t>(out.size());
            out.insert(out.end(), path.begin(), path.end());
            out.push_back(0);
            Wr32(out.data() + rec + kTexNameCount,  static_cast<uint32_t>(path.size()) + 1);
            Wr32(out.data() + rec + kTexNameOffset, nameOfs);
            ++inlined;
        }

        bool skelInlined = false;
        bool skelRead = false;
        SkelInlineStats skelStats = {};
        std::string skelPath;
        bool skelPathInferred = false;
        if (!skid.empty())
        {
            const uint32_t firstSkid = skid.front();
            const bool resolved = resolve && resolve(firstSkid, skelPath);
            if ((!resolved || skelPath.empty()) && !sourceName.empty())
            {
                skelPath = InferSkeletonPath(sourceName);
                skelPathInferred = !skelPath.empty();
            }
            if (!skelPath.empty() && readSidecar)
            {
                std::vector<uint8_t> skel;
                skelRead = readSidecar(firstSkid, skelPath, skel);
                if (skelRead && !skel.empty())
                {
                    skelInlined = InlineSkeleton(out, std::span<const uint8_t>(skel.data(), skel.size()), skelStats);
                    if (skelInlined)
                        wxl::core::log::Printf("modern-m2 md21: inlined SKID fdid=%u path=%s source=%s loops=%u seq=%u seqLookup=%u bones=%u boneLookup=%u skb=%u",
                            firstSkid, skelPath.c_str(), skelPathInferred ? "model-path" : "fdid",
                            skelStats.globalLoops, skelStats.sequences,
                            skelStats.sequenceLookup, skelStats.bones, skelStats.boneLookup, skelStats.skbBytes);
                    else
                        wxl::core::log::Printf("modern-m2 md21: failed to inline SKID fdid=%u path=%s source=%s",
                            firstSkid, skelPath.c_str(), skelPathInferred ? "model-path" : "fdid");
                }
            }
        }

        const uint32_t seqCount  = Rd32(out.data() + kHdrSequences);
        const uint32_t boneCount = Rd32(out.data() + kHdrBones);
        const uint32_t boneLookupCount = Rd32(out.data() + kHdrBoneLookup);
        const uint32_t boneComboCount = Rd32(out.data() + kHdrBoneCombos);
        wxl::core::log::Printf("modern-m2 md21: source=%.*s md20=%u seq=%u bones=%u boneLookup=%u boneCombos=%u textures=%u namedSlots=%u weaponBlade=%u inlined=%u txid=%zu skid=%zu skelRead=%u skelInlined=%u sfid=%u afid=%u bfid=%u",
            int(sourceName.size()), sourceName.data(), md20Size, seqCount, boneCount, boneLookupCount, boneComboCount, texCount, namedSlots,
            weaponBladeSlots, inlined, txid.size(), skid.size(), skelRead ? 1u : 0u, skelInlined ? 1u : 0u,
            sfidCount, afidCount, bfidCount);
        if (!skid.empty() && (seqCount == 0 || boneCount == 0))
        {
            const uint32_t firstSkid = skid.front();
            if (skelPath.empty() && resolve) resolve(firstSkid, skelPath);
            if (skelPath.empty() && !sourceName.empty()) skelPath = InferSkeletonPath(sourceName);
            wxl::core::log::Printf("modern-m2 md21: SKID sidecar required fdid=%u path=%s (skeleton/sequence sidecar was not available or could not be inlined)",
                firstSkid, skelPath.empty() ? "<unresolved>" : skelPath.c_str());
        }
        return true;
    }

    void ZeroBoneLookup(uint8_t* md20, uint32_t size)
    {
        // Zeroing routes every vertex to global bone 0, which the shadow-swing fix assumes is a static
        // root. When the lookup references a billboard bone that assumption fails: a model whose body
        // sits on a static bone but whose glow sits on a spherical-billboard bone 0 would billboard
        // wholesale (the whole lantern faces the camera, not just the glow). Leave such a model's lookup
        // intact so each part keeps its own bone; pure-static doodads (the swing case) still collapse.
        if (LookupReferencesBillboard(md20, size))
            return;

        const uint32_t count = Rd32(md20 + kHdrBoneCombos);
        const uint32_t ofs   = Rd32(md20 + kHdrBoneCombos + 4);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (ofs + i * 2 + 2 > size) break;
            Wr16(md20 + ofs + i * 2, 0);
        }
    }
}
