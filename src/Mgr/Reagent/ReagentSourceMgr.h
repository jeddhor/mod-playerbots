/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REAGENTSOURCEMGR_H
#define PLAYERBOTS_REAGENTSOURCEMGR_H

#include "Common.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

/**
 * P10.4 -- where a bot can go and farm a given reagent.
 *
 * The question this answers is "I need eight Linen Cloth, where do I get them", which is the piece
 * missing between knowing a recipe and being able to make it. Gathering nodes are already indexed by
 * GatherRouteMgr, so this covers what that does not: creature drops, skinning, fishing and chests.
 *
 * Three exclusions are applied at build time rather than at the point of use, so that no decision
 * path can forget one:
 *
 *  - anything whose only spawns are on a map in `instance_template`. That is what makes an Abyss
 *    Crystal correctly unfarmable -- it has exactly one loot row and no outdoor spawn -- and it
 *    falls out of the data rather than needing a hand-maintained list of raid reagents.
 *  - items with no surviving source at all, which may then only be bought.
 *  - sources far above the bot's level, filtered per bot at selection time.
 *
 * Only reagents are indexed. The manager exists to answer a question about crafting, and indexing
 * every lootable item in the game would be a hundred times the work for no additional answer.
 */
class ReagentSourceMgr
{
public:
    enum class Kind : uint8
    {
        Creature,
        Skinning,
        Fishing,
        GameObject
    };

    struct Source
    {
        Kind kind{Kind::Creature};
        uint32 sourceEntry{0};   //< creature or gameobject template entry
        uint32 mapId{0};
        uint32 zoneId{0};
        uint8 minLevel{0};
        uint8 maxLevel{0};
        float chance{0.0f};      //< percent; 0 means "came through a reference, rate unknown"
        uint32 spawnCount{0};
    };

    static ReagentSourceMgr& instance()
    {
        static ReagentSourceMgr instance;
        return instance;
    }

    /// Build the index. Safe to call more than once; the second call is a no-op.
    void Load();

    /// True if this item has at least one source a bot could reach.
    bool IsFarmable(uint32 itemId) const;

    /// Every known source, or nullptr. Ordered by how attractive they are to farm.
    std::vector<Source> const* SourcesFor(uint32 itemId) const;

    /**
     * The best place for this particular bot to farm this item, or nullptr if none is suitable.
     *
     * Prefers sources the bot can survive and that are dense enough to be worth the trip, and
     * strongly prefers the bot's current zone: a reagent available here beats a better rate a
     * continent away, because the travel is the expensive part.
     */
    Source const* BestFor(Player* bot, uint32 itemId) const;

    std::string DescribeStats() const;

private:
    ReagentSourceMgr() = default;
    ~ReagentSourceMgr() = default;

    ReagentSourceMgr(ReagentSourceMgr const&) = delete;
    ReagentSourceMgr& operator=(ReagentSourceMgr const&) = delete;

    /// Item ids that some known spell consumes. The index is built only for these.
    std::unordered_set<uint32> CollectReagentItems() const;

    mutable std::shared_mutex _mutex;
    bool _loaded{false};
    std::unordered_map<uint32, std::vector<Source>> _sources;

    uint32 _itemsIndexed{0};
    uint32 _sourcesIndexed{0};
    uint32 _rejectedInstanceOnly{0};
};

#define sReagentSourceMgr ReagentSourceMgr::instance()

#endif
