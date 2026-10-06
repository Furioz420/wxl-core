// Validated, server-authored cosmetic allowlist. No renderer or DB dependencies.
// GPL-2.0-or-later.
#pragma once
#include "AppearanceProtocol.hpp"
#include <map>
#include <set>
#include <sstream>
#include <string>
namespace wxl::appearance
{
class Catalog
{
    uint32_t version_ = 0;
    std::map<uint16_t, uint32_t> models_, alternateModels_;
    std::map<uint32_t, std::map<uint32_t, std::set<uint32_t>>> choices_;

  public:
    uint32_t VersionId() const
    {
        return version_;
    }
    uint32_t Model(uint8_t race, uint8_t sex) const
    {
        auto i = models_.find(uint16_t(race) * 2 + sex);
        return i == models_.end() ? 0 : i->second;
    }
    uint32_t AlternateModel(uint8_t race, uint8_t sex) const
    {
        auto i = alternateModels_.find(uint16_t(race) * 2 + sex);
        return i == alternateModels_.end() ? 0 : i->second;
    }
    std::vector<Choice> Defaults(uint32_t model) const
    {
        std::vector<Choice> result;
        auto i = choices_.find(model);
        if (i != choices_.end()) for (const auto &[option, choices] : i->second)
            if (!choices.empty()) result.push_back({option, *choices.begin()});
        return result;
    }
    bool UpgradeLegacy(Selection &s) const
    {
        if (s.catalog != LegacyCatalogId || !s.alternate.empty()) return false;
        auto next = s;
        next.catalog = version_;
        if (next.race == 23 && next.sex == 1) next.choices.push_back({PrivateHornStyle, 2});
        if (AlternateRetailRace(next.race)) next.alternate = Defaults(AlternateModel(next.race, next.sex));
        if (!Validate(next)) return false;
        s = std::move(next);
        return true;
    }
    bool Load(std::istream &input, std::string &error)
    {
        Catalog next;
        std::string line;
        uint32_t rows = 0;
        bool header = false;
        while (std::getline(input, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            if (line.size() > 256 || ++rows > 200000)
            {
                error = "catalog limits exceeded";
                return false;
            }
            std::istringstream row(line);
            std::string kind, extra;
            row >> kind;
            if (!header)
            {
                uint32_t protocol = 0, map = 0;
                if (kind != "WXLAPPEARANCE" || !(row >> protocol >> next.version_ >> map) ||
                    protocol != Version || !next.version_ || map != RaceMapVersion || (row >> extra))
                {
                    error = "invalid catalog header";
                    return false;
                }
                header = true;
                continue;
            }
            uint32_t a = 0, b = 0, c = 0;
            if (!(row >> a >> b >> c) || (row >> extra))
            {
                error = "invalid catalog row";
                return false;
            }
            if (kind == "I")
            {
                if (a > 32 || !FindRace(uint8_t(a)) || b > 1 || !c ||
                    !next.models_.emplace(uint16_t(a * 2 + b), c).second)
                {
                    error = "invalid or duplicate identity";
                    return false;
                }
            }
            else if (kind == "A")
            {
                if (a > 32 || !AlternateRetailRace(uint8_t(a)) || b > 1 || !c ||
                    !next.alternateModels_.emplace(uint16_t(a * 2 + b), c).second)
                { error = "invalid alternate identity"; return false; }
            }
            else if (kind == "C")
            {
                if (!a || !b || !c || !next.choices_[a][b].insert(c).second)
                {
                    error = "invalid or duplicate choice";
                    return false;
                }
            }
            else
            {
                error = "unknown row kind";
                return false;
            }
        }
        if (!header || next.models_.empty())
        {
            error = "empty catalog";
            return false;
        }
        for (auto [identity, model] : next.models_)
        {
            auto i = next.choices_.find(model);
            if (i == next.choices_.end() || i->second.empty() || i->second.size() > MaxChoices)
            {
                error = "incomplete or oversized model";
                return false;
            }
        }
        for (auto [identity, model] : next.alternateModels_)
        {
            auto i = next.choices_.find(model);
            if (!next.models_.count(identity) || i == next.choices_.end() || i->second.empty() || i->second.size() > MaxChoices)
            { error = "incomplete alternate model"; return false; }
        }
        for (auto [identity, model] : next.models_)
            if (AlternateRetailRace(uint8_t(identity / 2)) && !next.alternateModels_.count(identity))
            { error = "missing alternate identity"; return false; }
        *this = std::move(next);
        return true;
    }
    bool Validate(Selection s) const
    {
        if (s.catalog != version_ || !Canonical(s))
            return false;
        auto valid = [&](uint32_t model, const std::vector<Choice> &values) {
            auto m = choices_.find(model);
            if (m == choices_.end() || values.size() != m->second.size()) return false;
            for (auto c : values) {
                auto o = m->second.find(c.option);
                if (o == m->second.end() || !o->second.count(c.choice)) return false;
            }
            return true;
        };
        if (!valid(Model(s.race, s.sex), s.choices)) return false;
        if (AlternateRetailRace(s.race) && !valid(AlternateModel(s.race, s.sex), s.alternate)) return false;
        return true;
    }
};
} // namespace wxl::appearance
