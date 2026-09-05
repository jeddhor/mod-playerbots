/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherRouteMgr.h"
#include "DBCStores.h"
// NOTE: do not include GameObjectData.h directly - it declares a G3D::Quat member without
// including G3D itself, so it only compiles behind ObjectMgr.h.
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RandomUtils.h"
#include "Timer.h"
#include <algorithm>
#include <cmath>

namespace
{
// A route longer than this is a chore rather than a circuit; shorter than this is not worth the
// travel to reach it.
constexpr size_t MIN_ROUTE_NODES = 6;
constexpr size_t MAX_ROUTE_NODES = 15;
// Nodes further apart than this belong to different clusters, not the same loop.
constexpr float MAX_LINK_DISTANCE = 300.0f;
// Spawn points within this radius collapse into one waypoint. Comfortably inside the bot's node
// detection range, so standing on the centroid puts the whole cluster in view.
constexpr float CLUSTER_RADIUS = 50.0f;

float Dist2(GatherRouteMgr::Node const& a, GatherRouteMgr::Node const& b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return dx * dx + dy * dy;
}
}  // namespace

void GatherRouteMgr::Load()
{
    uint32 oldMSTime = getMSTime();
    _routes.clear();
    _byZone.clear();

    // (zoneId, skillId) -> nodes
    std::map<std::pair<uint32, uint32>, std::vector<Node>> buckets;

    for (auto const& pair : sObjectMgr->GetAllGOData())
    {
        GameObjectData const& data = pair.second;

        GameObjectTemplate const* tmpl = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!tmpl)
            continue;

        uint32 lockId = tmpl->GetLockId();
        if (!lockId)
            continue;

        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            continue;

        // Same resolution LootObject performs at runtime: find a lock index that is a gathering
        // skill rather than a key or a trap.
        uint32 skillId = 0;
        uint32 reqSkillValue = 0;
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_SKILL)
                continue;

            LockType lockType = LockType(lock->Index[i]);
            if (lockType != LOCKTYPE_HERBALISM && lockType != LOCKTYPE_MINING)
                continue;

            skillId = SkillByLockType(lockType);
            reqSkillValue = lock->Skill[i];
            break;
        }

        if (!skillId)
            continue;

        uint32 zoneId = sMapMgr->GetZoneId(data.phaseMask, data.mapid, data.posX, data.posY, data.posZ);
        if (!zoneId)
            continue;

        buckets[{zoneId, skillId}].push_back(
            Node{data.mapid, data.posX, data.posY, data.posZ, reqSkillValue});
    }

    uint32 rawPoints = 0;
    uint32 waypoints = 0;
    for (auto& entry : buckets)
    {
        rawPoints += static_cast<uint32>(entry.second.size());
        std::vector<Node> clustered = ClusterSpawnPoints(entry.second);
        waypoints += static_cast<uint32>(clustered.size());
        BuildRoutesForBucket(entry.first.first, entry.first.second, clustered);
    }

    for (uint32 i = 0; i < _routes.size(); ++i)
        _byZone[_routes[i].zoneId].push_back(i);

    LOG_INFO("server.loading",
             ">> Loaded {} playerbot gathering routes across {} zones ({} spawn points -> {} clustered waypoints) "
             "in {} ms",
             _routes.size(), _byZone.size(), rawPoints, waypoints, GetMSTimeDiffToNow(oldMSTime));
}

std::vector<GatherRouteMgr::Node> GatherRouteMgr::ClusterSpawnPoints(std::vector<Node>& points) const
{
    std::vector<Node> clusters;
    std::vector<bool> used(points.size(), false);

    for (size_t i = 0; i < points.size(); ++i)
    {
        if (used[i])
            continue;

        Node centroid = points[i];
        double sx = points[i].x, sy = points[i].y, sz = points[i].z;
        uint32 count = 1;
        used[i] = true;

        for (size_t j = i + 1; j < points.size(); ++j)
        {
            if (used[j] || points[j].mapId != points[i].mapId)
                continue;

            if (Dist2(points[i], points[j]) > CLUSTER_RADIUS * CLUSTER_RADIUS)
                continue;

            sx += points[j].x;
            sy += points[j].y;
            sz += points[j].z;
            // The cluster is only workable if the bot can harvest its hardest member.
            centroid.requiredSkillValue = std::max(centroid.requiredSkillValue, points[j].requiredSkillValue);
            ++count;
            used[j] = true;
        }

        centroid.x = static_cast<float>(sx / count);
        centroid.y = static_cast<float>(sy / count);
        centroid.z = static_cast<float>(sz / count);
        centroid.spawnPoints = count;
        clusters.push_back(centroid);
    }

    // Denser clusters first: with ~1.4 of every 4 pooled points live, a waypoint covering more
    // spawn points is proportionally more likely to have something actually there.
    std::sort(clusters.begin(), clusters.end(),
              [](Node const& a, Node const& b) { return a.spawnPoints > b.spawnPoints; });

    return clusters;
}

void GatherRouteMgr::BuildRoutesForBucket(uint32 zoneId, uint32 skillId, std::vector<Node>& nodes)
{
    // Greedy nearest-neighbour: start anywhere, repeatedly hop to the closest unused node, and cut
    // when the loop is long enough or the next node is too far to belong to the same cluster.
    // Deliberately not an optimal TSP - the bot is wandering a field picking herbs, and a
    // near-enough ordering reads exactly the same in game for a fraction of the cost.
    std::vector<bool> used(nodes.size(), false);
    size_t remaining = nodes.size();

    while (remaining >= MIN_ROUTE_NODES)
    {
        size_t start = 0;
        while (start < nodes.size() && used[start])
            ++start;
        if (start >= nodes.size())
            break;

        Route route;
        route.zoneId = zoneId;
        route.skillId = skillId;
        route.nodes.push_back(nodes[start]);
        route.minSkillValue = nodes[start].requiredSkillValue;
        used[start] = true;
        --remaining;

        while (route.nodes.size() < MAX_ROUTE_NODES && remaining)
        {
            Node const& from = route.nodes.back();
            size_t best = nodes.size();
            float bestD2 = MAX_LINK_DISTANCE * MAX_LINK_DISTANCE;

            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (used[i] || nodes[i].mapId != from.mapId)
                    continue;

                float d2 = Dist2(from, nodes[i]);
                if (d2 < bestD2)
                {
                    bestD2 = d2;
                    best = i;
                }
            }

            if (best == nodes.size())
                break;  // nothing close enough: this cluster is finished

            route.minSkillValue = std::max(route.minSkillValue, nodes[best].requiredSkillValue);
            route.nodes.push_back(nodes[best]);
            used[best] = true;
            --remaining;
        }

        if (route.nodes.size() >= MIN_ROUTE_NODES)
            _routes.push_back(std::move(route));
    }
}

std::vector<GatherRouteMgr::Route const*> GatherRouteMgr::GetRoutesFor(Player* bot, uint32 zoneId) const
{
    std::vector<Route const*> out;
    if (!bot)
        return out;

    auto itr = _byZone.find(zoneId);
    if (itr == _byZone.end())
        return out;

    for (uint32 idx : itr->second)
    {
        Route const& route = _routes[idx];

        uint32 skill = bot->GetSkillValue(route.skillId);
        if (!skill)
            continue;  // bot does not have this profession at all

        // Must be able to harvest the hardest node on the route, or it walks past most of it.
        if (skill < route.minSkillValue)
            continue;

        out.push_back(&route);
    }

    return out;
}

GatherRouteMgr::Route const* GatherRouteMgr::PickRoute(Player* bot, uint32 zoneId) const
{
    std::vector<Route const*> candidates = GetRoutesFor(bot, zoneId);
    Route const* const* picked = RandomElement(candidates);
    return picked ? *picked : nullptr;
}
