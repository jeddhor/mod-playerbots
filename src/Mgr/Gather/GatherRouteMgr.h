/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GATHERROUTEMGR_H
#define PLAYERBOTS_GATHERROUTEMGR_H

#include "Define.h"
#include "SharedDefines.h"
#include <unordered_map>
#include <vector>

class Player;

/**
 * Precomputed gathering routes: ordered loops of herb or mineral nodes, grouped by zone and skill.
 *
 * The harvesting mechanics already work - `GatherStrategy`, `AddGatheringLootAction` and
 * `LootObjectStack` will happily skin, mine and pick anything that comes within loot range. What is
 * missing is intent: today a bot only gathers what it happens to walk past while doing something
 * else. This supplies the "go to where the ore is" half.
 *
 * Built once at startup from spawn data, using the same lock->skill chain LootObject uses at
 * runtime: GameObjectTemplate::GetLockId() -> sLockStore -> SkillByLockType. No live objects are
 * needed, so the index is available before any bot logs in.
 */
class GatherRouteMgr
{
public:
    static GatherRouteMgr& instance()
    {
        static GatherRouteMgr instance;
        return instance;
    }

    /// A waypoint: the CENTROID of a cluster of nearby spawn points, not a single spawn point.
    ///
    /// Herb and ore nodes are pooled - roughly 4 candidate spawn points share a pool and only ~1.4
    /// are live at any moment (measured: 29,862 of 31,592 gatherable spawn points are pooled). An
    /// index built from individual spawn points therefore sends bots to empty ground about two
    /// thirds of the time, which is exactly what the first behavioural test showed: 21 bots
    /// gathering for 35 minutes produced 7 items.
    ///
    /// Aiming at a cluster centroid instead makes pooling irrelevant. The bot stands where nodes
    /// *tend* to be and the existing `gather` strategy picks up whatever actually spawned, since
    /// the whole cluster sits well inside its detection range.
    struct Node
    {
        uint32 mapId{0};
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        uint32 requiredSkillValue{0};
        uint32 spawnPoints{1};   ///< how many pooled spawn points this centroid covers
    };

    /// An ordered loop a bot walks in sequence.
    struct Route
    {
        uint32 zoneId{0};
        uint32 skillId{0};
        // The gate to walk the route: the MEDIAN node requirement, so a bot that takes it can
        // harvest at least half of what it walks past.
        //
        // This was originally the route's highest requirement, which was wrong and silently
        // disabled gathering entirely. Routes are clustered by position, not by tier, so a single
        // Mithril vein among fifteen Copper nodes gated the whole route at 175 and no low-skill
        // bot could ever take it. Zero routes were reachable in practice.
        uint32 gateSkillValue{0};
        uint32 maxSkillValue{0};    // hardest node, kept for reporting
        std::vector<Node> nodes;
    };

    /// Build the index. Call once during world startup, after gameobject data is loaded.
    void Load();

    /// Routes for a zone that this bot's skill can actually harvest, or empty.
    std::vector<Route const*> GetRoutesFor(Player* bot, uint32 zoneId) const;

    /// Pick one route at random from those the bot qualifies for. nullptr if none.
    Route const* PickRoute(Player* bot, uint32 zoneId) const;

    /**
     * A route the bot could work, preferring its current zone but willing to travel.
     *
     * Bots spend a lot of time in cities -- resting at innkeepers is one of the commonest RPG
     * activities -- and cities correctly contain nothing to gather. Over a three hour run, 904 of
     * 1889 availability checks were refused for "no usable route in this zone", led by Dalaran,
     * Stormwind, Shattrath and Orgrimmar. Requiring a node underfoot means a gatherer standing in a
     * capital can never decide to go gathering, which is not how anyone plays.
     *
     * Restricted to the bot's current map and a distance cap so this stays "walk to the hills
     * outside town", not "cross the world".
     */
    Route const* PickRouteWithinReach(Player* bot) const;

    uint32 GetRouteCount() const { return static_cast<uint32>(_routes.size()); }

private:
    GatherRouteMgr() = default;
    ~GatherRouteMgr() = default;
    GatherRouteMgr(GatherRouteMgr const&) = delete;
    GatherRouteMgr& operator=(GatherRouteMgr const&) = delete;

    /// Greedy nearest-neighbour walk over a zone's nodes, cut into loops of a workable length.
    /// Collapse nearby spawn points into centroids before routing. See Node.
    std::vector<Node> ClusterSpawnPoints(std::vector<Node>& points) const;
    void BuildRoutesForBucket(uint32 zoneId, uint32 skillId, std::vector<Node>& nodes);

    std::vector<Route> _routes;
    // zoneId -> indices into _routes, so lookup at pick time is a single hash.
    std::unordered_map<uint32, std::vector<uint32>> _byZone;
};

#define sGatherRouteMgr GatherRouteMgr::instance()

#endif
