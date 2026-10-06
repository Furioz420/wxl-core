// WXL appearance wire contract. Shared verbatim by client and server. GPL-2.0-or-later.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace wxl::appearance
{
constexpr uint8_t Version = 2;
constexpr uint16_t MaxChoices = 64;
constexpr uint16_t CmsgHello = 0x540, SmsgHello = 0x541, CmsgPrepare = 0x542, CmsgQuery = 0x543,
                   SmsgSnapshot = 0x544, SmsgPrepared = 0x545;
constexpr uint32_t RaceMapVersion = 20260907;
struct Race
{
    uint8_t id;
    uint16_t retail;
    uint8_t team;
    bool creatable;
};
// team follows ChrRaces.TeamID (7 Alliance, 1 Horde). 25 remains reserved.
constexpr Race Races[] = {{1, 1, 7, true},    {2, 2, 1, true},   {3, 3, 7, true},   {4, 4, 7, true},
                          {5, 5, 1, true},    {6, 6, 1, true},   {7, 7, 7, true},   {8, 8, 1, true},
                          {9, 35, 1, true},   {10, 10, 1, true}, {11, 11, 7, true}, {12, 22, 7, true},
                          {13, 27, 1, true},  {14, 17, 7, true}, {15, 29, 7, true}, {16, 0, 1, true},
                          {17, 76, 1, true},  {18, 31, 1, true}, {19, 0, 1, true},  {20, 30, 7, true},
                          {21, 9, 1, true},   {22, 26, 1, true}, {23, 14, 7, true}, {24, 85, 7, true},
                          {25, 13, 7, false}, {26, 25, 7, true}, {27, 86, 1, true}, {28, 0, 1, true},
                          {29, 34, 7, true},  {30, 75, 7, true}, {31, 32, 7, true}, {32, 0, 7, false}};
constexpr const Race *FindRace(uint8_t id)
{
    for (const auto &r : Races)
        if (r.id == id)
            return &r;
    return nullptr;
}
constexpr uint32_t RaceBit(uint8_t id)
{
    return id >= 1 && id <= 32 ? uint32_t(1) << (id - 1) : 0;
}
constexpr uint32_t LegacyCatalogId = 4256143482u;
constexpr uint16_t AlternateRetailRace(uint8_t race)
{
    return race == 12 ? 23 : race == 17 ? 70 : race == 30 ? 52 : 0;
}
constexpr uint32_t PrivateHornStyle = 0x80000004u;
constexpr uint32_t PrivateSkin = 0x80000001u, PrivateOutfit = 0x80000002u, PrivateHairColor = 0x80000003u;
constexpr uint32_t PrivateModel(uint8_t race, uint8_t sex)
{
    return ((race == 23 && sex < 2) || (sex == 1 && (race == 14 || race == 28)))
               ? 0x80000000u + uint32_t(race) * 2 + sex
               : 0;
}
struct Choice
{
    uint32_t option = 0, choice = 0;
    bool operator==(const Choice &o) const
    {
        return option == o.option && choice == o.choice;
    }
};
struct Selection
{
    uint32_t catalog = 0;
    uint8_t race = 0, sex = 0;
    std::vector<Choice> choices;
    std::vector<Choice> alternate;
};
struct Snapshot
{
    uint64_t guid = 0;
    uint32_t revision = 0;
    Selection selection;
};
class Reader
{
    const uint8_t *p;
    size_t n, at = 0;

  public:
    Reader(const uint8_t *bytes, size_t size) : p(bytes), n(size)
    {
    }
    template <class T> bool Read(T &v)
    {
        if (!p || sizeof(T) > n - at)
            return false;
        v = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            v |= T(p[at++]) << (i * 8);
        return true;
    }
    size_t Remaining() const
    {
        return n - at;
    }
};
template <class T> inline void Put(std::vector<uint8_t> &out, T v)
{
    // Shift a wide accumulator even when T is a one-byte wire field.
    uint64_t remaining = static_cast<uint64_t>(v);
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        out.push_back(uint8_t(remaining & 255));
        remaining >>= 8;
    }
}
inline bool Canonical(Selection &s)
{
    if (!s.catalog || !FindRace(s.race) || s.sex > 1 || s.choices.size() > MaxChoices || s.alternate.size() > MaxChoices ||
        (!AlternateRetailRace(s.race) && !s.alternate.empty()))
        return false;
    std::sort(s.choices.begin(), s.choices.end(), [](auto a, auto b) { return a.option < b.option; });
    uint32_t prev = 0;
    for (auto c : s.choices)
    {
        if (!c.option || !c.choice || c.option == prev)
            return false;
        prev = c.option;
    }
    std::sort(s.alternate.begin(), s.alternate.end(), [](auto a, auto b) { return a.option < b.option; });
    prev = 0;
    for (auto c : s.alternate)
    {
        if (!c.option || !c.choice || c.option == prev) return false;
        prev = c.option;
    }
    return true;
}
inline bool ReadSelection(Reader &r, Selection &out)
{
    Selection s;
    uint16_t count = 0;
    if (!r.Read(s.catalog) || !r.Read(s.race) || !r.Read(s.sex) || !r.Read(count) || count > MaxChoices ||
        r.Remaining() < size_t(count) * 8)
        return false;
    s.choices.resize(count);
    for (auto &c : s.choices)
        if (!r.Read(c.option) || !r.Read(c.choice))
            return false;
    if (!r.Read(count) || count > MaxChoices || r.Remaining() < size_t(count) * 8) return false;
    s.alternate.resize(count);
    for (auto &c : s.alternate)
        if (!r.Read(c.option) || !r.Read(c.choice)) return false;
    if (!Canonical(s))
        return false;
    out = std::move(s);
    return true;
}
inline void PutSelection(std::vector<uint8_t> &out, const Selection &s)
{
    Put(out, s.catalog);
    Put(out, s.race);
    Put(out, s.sex);
    Put(out, uint16_t(s.choices.size()));
    for (auto c : s.choices)
    {
        Put(out, c.option);
        Put(out, c.choice);
    }
    Put(out, uint16_t(s.alternate.size()));
    for (auto c : s.alternate)
    {
        Put(out, c.option);
        Put(out, c.choice);
    }
}
inline bool DecodeSnapshot(const uint8_t *p, size_t n, Snapshot &out)
{
    Reader r(p, n);
    uint8_t v = 0;
    Snapshot s;
    if (!r.Read(v) || v != Version || !r.Read(s.guid) || !s.guid || !r.Read(s.revision) ||
        !ReadSelection(r, s.selection) || r.Remaining())
        return false;
    out = std::move(s);
    return true;
}
inline std::vector<uint8_t> EncodeSnapshot(const Snapshot &s)
{
    std::vector<uint8_t> b;
    Put(b, Version);
    Put(b, s.guid);
    Put(b, s.revision);
    PutSelection(b, s.selection);
    return b;
}
} // namespace wxl::appearance
