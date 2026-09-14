/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReagentSourceMgr.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "MapMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>

namespace
{
//: A source with fewer spawns than this is a curiosity rather than a farm: the bot would spend the
//: whole trip walking between the two creatures that have it.
constexpr uint32 MIN_USEFUL_SPAWNS = 4;

//: How far above the bot a source's creatures may be before the trip is a death rather than a farm.
constexpr int32 MAX_LEVEL_MARGIN = 3;

/// Loot rows that reach an item through reference_loot_template, flattened to (lootId -> items).
///
/// Necessary rather than optional: a reference row carries the item id of a *label*, not of a real
/// item, so reading it directly indexes items that do not drop while missing the ones that do.
void LoadLootTable(char const* table, std::unordered_map<uint32, std::vector<std::pair<uint32, float>>>& out)
{
    QueryResult result = WorldDatabase.Query(
        "SELECT lt.Entry, "
        "       IF(lt.Reference = 0, lt.Item, rlt.Item), "
        "       IF(lt.Reference = 0, lt.Chance, 0) "
        "FROM {} lt "
        "LEFT JOIN reference_loot_template rlt ON lt.Reference <> 0 AND rlt.Entry = lt.Reference "
        "WHERE IF(lt.Reference = 0, lt.Item, rlt.Item) > 0",
        table);

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        out[fields[0].Get<uint32>()].emplace_back(fields[1].Get<uint32>(), fields[2].Get<float>());
    } while (result->NextRow());
}
}  // namespace

std::unordered_set<uint32> ReagentSourceMgr::CollectReagentItems() const
{
    std::unordered_set<uint32> reagents;

    for (uint32 spellId = 0; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (info->Reagent[i] > 0 && info->ReagentCount[i] > 0)
                reagents.insert(uint32(info->Reagent[i]));
    }

    return reagents;
}

void ReagentSourceMgr::Load()
{
    {
        std::shared_lock<std::shared_mutex> guard(_mutex);
        if (_loaded)
            return;
    }

    uint32 const started = getMSTime();

    std::unordered_set<uint32> const reagents = CollectReagentItems();
    if (reagents.empty())
    {
        LOG_WARN("playerbots", "[Reagents] no spell reagents found; the index will be empty");
        return;
    }

    // Instance maps, excluded wholesale. This is the rule that makes raid reagents unfarmable
    // without anybody having to list them.
    std::unordered_set<uint32> instanceMaps;
    if (QueryResult result = WorldDatabase.Query("SELECT map FROM instance_template"))
        do
        {
            instanceMaps.insert(result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());

    std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> creatureLoot;
    std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> skinLoot;
    std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> objectLoot;
    LoadLootTable("creature_loot_template", creatureLoot);
    LoadLootTable("skinning_loot_template", skinLoot);
    LoadLootTable("gameobject_loot_template", objectLoot);

    std::unordered_map<uint32, std::vector<Source>> built;
    uint32 rejectedInstanceOnly = 0;

    // Creatures and skinning share a shape: template -> loot id -> items, spawns -> map and zone.
    struct SpawnInfo
    {
        uint32 mapId{0};
        uint32 zoneId{0};
        uint32 count{0};
    };

    auto indexCreatures = [&](std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> const& loot,
                              char const* lootColumn, Kind kind)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT ct.entry, ct.{}, ct.minlevel, ct.maxlevel, c.map, c.position_x, c.position_y, c.position_z "
            "FROM creature_template ct JOIN creature c ON c.id = ct.entry "
            "WHERE ct.{} > 0",
            lootColumn, lootColumn);

        if (!result)
            return;

        // (creature, zone) -> spawn count, so a creature spread over three zones becomes three
        // sources rather than one averaged blur.
        std::unordered_map<uint64, SpawnInfo> byCreatureZone;
        std::unordered_map<uint32, std::pair<uint32, std::pair<uint8, uint8>>> creatureLootAndLevel;
        bool anyOutdoor = false;

        do
        {
            Field* fields = result->Fetch();
            uint32 const entry = fields[0].Get<uint32>();
            uint32 const lootId = fields[1].Get<uint32>();
            uint8 const minLevel = fields[2].Get<uint8>();
            uint8 const maxLevel = fields[3].Get<uint8>();
            uint32 const mapId = fields[4].Get<uint32>();

            if (instanceMaps.count(mapId))
                continue;

            anyOutdoor = true;

            // Zone from map and position, never from creature.zoneId: that column is 0 on 144,944 of
            // this realm's 150,063 spawns, so trusting it would file almost every source under zone 0.
            uint32 const zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, mapId, fields[5].Get<float>(),
                                                     fields[6].Get<float>(), fields[7].Get<float>());

            uint64 const key = (uint64(entry) << 32) | zoneId;
            SpawnInfo& info = byCreatureZone[key];
            info.mapId = mapId;
            info.zoneId = zoneId;
            ++info.count;

            creatureLootAndLevel[entry] = {lootId, {minLevel, maxLevel}};
        } while (result->NextRow());

        if (!anyOutdoor)
            return;

        for (auto const& [key, spawn] : byCreatureZone)
        {
            uint32 const entry = uint32(key >> 32);
            auto const meta = creatureLootAndLevel.find(entry);
            if (meta == creatureLootAndLevel.end())
                continue;

            auto const items = loot.find(meta->second.first);
            if (items == loot.end())
                continue;

            for (auto const& [itemId, chance] : items->second)
            {
                if (!reagents.count(itemId))
                    continue;

                built[itemId].push_back(Source{kind, entry, spawn.mapId, spawn.zoneId,
                                               meta->second.second.first, meta->second.second.second,
                                               chance, spawn.count});
            }
        }
    };

    indexCreatures(creatureLoot, "lootid", Kind::Creature);
    indexCreatures(skinLoot, "skinloot", Kind::Skinning);

    // Chests and fishing holes. gameobject_template.Data1 is the loot id, and only for types 3 and
    // 25 -- on a door or a button that column means something else entirely.
    if (QueryResult result = WorldDatabase.Query(
            "SELECT gt.entry, gt.Data1, g.map, g.position_x, g.position_y, g.position_z "
            "FROM gameobject_template gt JOIN gameobject g ON g.id = gt.entry "
            "WHERE gt.type IN (3, 25) AND gt.Data1 > 0"))
    {
        std::unordered_map<uint64, SpawnInfo> byObjectZone;
        std::unordered_map<uint32, uint32> objectLootId;

        do
        {
            Field* fields = result->Fetch();
            uint32 const entry = fields[0].Get<uint32>();
            uint32 const mapId = fields[2].Get<uint32>();

            if (instanceMaps.count(mapId))
                continue;

            uint32 const zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, mapId, fields[3].Get<float>(),
                                                     fields[4].Get<float>(), fields[5].Get<float>());

            uint64 const key = (uint64(entry) << 32) | zoneId;
            SpawnInfo& info = byObjectZone[key];
            info.mapId = mapId;
            info.zoneId = zoneId;
            ++info.count;
            objectLootId[entry] = fields[1].Get<uint32>();
        } while (result->NextRow());

        for (auto const& [key, spawn] : byObjectZone)
        {
            uint32 const entry = uint32(key >> 32);
            auto const lootId = objectLootId.find(entry);
            if (lootId == objectLootId.end())
                continue;

            auto const items = objectLoot.find(lootId->second);
            if (items == objectLoot.end())
                continue;

            for (auto const& [itemId, chance] : items->second)
                if (reagents.count(itemId))
                    built[itemId].push_back(
                        Source{Kind::GameObject, entry, spawn.mapId, spawn.zoneId, 0, 0, chance, spawn.count});
        }
    }

    // Fishing is keyed by zone directly rather than by a spawn, so it needs no position lookup.
    if (QueryResult result = WorldDatabase.Query(
            "SELECT Entry, Item, Chance FROM fishing_loot_template WHERE Reference = 0 AND Item > 0"))
        do
        {
            Field* fields = result->Fetch();
            uint32 const itemId = fields[1].Get<uint32>();
            if (!reagents.count(itemId))
                continue;

            uint32 const zoneId = fields[0].Get<uint32>();
            built[itemId].push_back(Source{Kind::Fishing, 0, 0, zoneId, 0, 0, fields[2].Get<float>(), 1});
        } while (result->NextRow());

    // Densest first, so BestFor's per-bot filtering starts from the most attractive.
    uint32 sources = 0;
    for (auto& [itemId, list] : built)
    {
        std::sort(list.begin(), list.end(), [](Source const& a, Source const& b) {
            if (a.spawnCount != b.spawnCount)
                return a.spawnCount > b.spawnCount;
            return a.chance > b.chance;
        });
        sources += uint32(list.size());
    }

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _sources = std::move(built);
        _itemsIndexed = uint32(_sources.size());
        _sourcesIndexed = sources;
        _rejectedInstanceOnly = rejectedInstanceOnly;
        _loaded = true;
    }

    LOG_INFO("playerbots", "[Reagents] indexed {} farmable reagents from {} sources in {}ms ({} reagents known)",
             _itemsIndexed, _sourcesIndexed, GetMSTimeDiffToNow(started), uint32(reagents.size()));
}

bool ReagentSourceMgr::IsFarmable(uint32 itemId) const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return _sources.count(itemId) != 0;
}

std::vector<ReagentSourceMgr::Source> const* ReagentSourceMgr::SourcesFor(uint32 itemId) const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    auto const itr = _sources.find(itemId);
    return itr != _sources.end() ? &itr->second : nullptr;
}

ReagentSourceMgr::Source const* ReagentSourceMgr::BestFor(Player* bot, uint32 itemId) const
{
    if (!bot)
        return nullptr;

    std::shared_lock<std::shared_mutex> guard(_mutex);
    auto const itr = _sources.find(itemId);
    if (itr == _sources.end())
        return nullptr;

    int32 const botLevel = int32(bot->GetLevel());
    uint32 const here = bot->GetZoneId();

    Source const* best = nullptr;
    float bestScore = 0.0f;

    for (Source const& source : itr->second)
    {
        if (source.minLevel && int32(source.minLevel) > botLevel + MAX_LEVEL_MARGIN)
            continue;

        if (source.spawnCount < MIN_USEFUL_SPAWNS && source.kind != Kind::Fishing)
            continue;

        // Density and drop rate, then a large thumb on the scale for being here already. Travel is
        // the expensive part of farming, so a mediocre source in this zone beats a better one on
        // another continent by a wide margin.
        float score = float(source.spawnCount) * std::max(source.chance, 1.0f);
        if (source.zoneId == here)
            score *= 10.0f;

        if (score > bestScore)
        {
            bestScore = score;
            best = &source;
        }
    }

    return best;
}

std::string ReagentSourceMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("reagents: {} farmable items, {} sources indexed", _itemsIndexed, _sourcesIndexed);
}
