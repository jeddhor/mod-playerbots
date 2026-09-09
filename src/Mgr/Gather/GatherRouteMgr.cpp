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

    // How many routes a bot at a given skill level can actually take. This is the number that
    // matters: routes that exist but are gated above every bot's skill are the same as no routes at
    // all, and that failure is invisible without printing it. A run where the low-skill buckets are
    // zero means gathering is disabled in practice no matter what the route count says.
    for (uint32 skill : {1u, 75u, 150u, 225u, 300u, 450u})
    {
        uint32 reachable = 0;
        for (Route const& route : _routes)
            if (skill >= route.gateSkillValue)
                ++reachable;

        LOG_INFO("server.loading", ">> Gathering: {} of {} routes reachable at skill {}", reachable,
                 _routes.size(), skill);
    }
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
        route.maxSkillValue = nodes[start].requiredSkillValue;
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

            route.maxSkillValue = std::max(route.maxSkillValue, nodes[best].requiredSkillValue);
            route.nodes.push_back(nodes[best]);
            used[best] = true;
            --remaining;
        }

        if (route.nodes.size() >= MIN_ROUTE_NODES)
        {
            // Median requirement, so the gate reflects what the route is mostly made of rather than
            // its single hardest node.
            std::vector<uint32> requirements;
            requirements.reserve(route.nodes.size());
            for (Node const& node : route.nodes)
                requirements.push_back(node.requiredSkillValue);

            std::nth_element(requirements.begin(), requirements.begin() + requirements.size() / 2,
                             requirements.end());
            route.gateSkillValue = requirements[requirements.size() / 2];

            _routes.push_back(std::move(route));
        }
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

        // Must be able to harvest the median node, i.e. at least half the route. Gating on the
        // hardest node instead means one high-tier vein in a spatial cluster locks out every bot
        // below it -- which disabled gathering completely.
        if (skill < route.gateSkillValue)
            continue;

        out.push_back(&route);
    }

    return out;
}

GatherRouteMgr::Route const* GatherRouteMgr::PickRouteWithinReach(Player* bot) const
{
    if (!bot)
        return nullptr;

    // Distance, not zone. Preferring the bot's own zone first looks sensible and is not: zones are
    // enormous, so a same-zone route was routinely further away than a neighbouring zone's. Measured
    // over 64 activity starts, the chosen route averaged 962 yards and reached 2558 -- against a
    // fifteen minute window, which is why availability could rise 16 points while the number of bots
    // that actually reached a node stayed at one.
    // Preferred reach, and a further one used only when nothing is near.
    //
    // 800 alone denied three gather attempts in four: 160 of 162 refusals were "no usable route
    // within reach", not a skill or bag problem. A bot standing where no route is close then did
    // nothing at all, which is worse than a walk -- the activity lasts twenty-five minutes and a
    // bot covers three thousand yards in about seven of them, so the journey is affordable.
    //
    // The near cap still comes first, so a bot with a vein next to it does not set off across the
    // zone. The far one only applies when the alternative is not gathering.
    constexpr float MAX_TRAVEL_DISTANCE = 800.0f;
    constexpr float MAX_TRAVEL_DISTANCE_SQ = MAX_TRAVEL_DISTANCE * MAX_TRAVEL_DISTANCE;
    constexpr float FAR_TRAVEL_DISTANCE = 3000.0f;
    constexpr float FAR_TRAVEL_DISTANCE_SQ = FAR_TRAVEL_DISTANCE * FAR_TRAVEL_DISTANCE;

    // Choose among the closest few rather than the single closest, so a hundred bots in one city do
    // not all descend on the same copper vein.
    constexpr size_t CANDIDATE_POOL = 5;

    uint32 const mapId = bot->GetMapId();
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    std::vector<std::pair<float, Route const*>> reachable;
    std::vector<std::pair<float, Route const*>> distant;

    for (Route const& route : _routes)
    {
        if (route.nodes.empty())
            continue;

        uint32 const skill = bot->GetSkillValue(route.skillId);
        if (!skill || skill < route.gateSkillValue)
            continue;

        Node const& first = route.nodes.front();
        if (first.mapId != mapId)
            continue;

        float const dx = first.x - botX;
        float const dy = first.y - botY;
        float const d2 = dx * dx + dy * dy;

        if (d2 <= MAX_TRAVEL_DISTANCE_SQ)
            reachable.emplace_back(d2, &route);
        else if (d2 <= FAR_TRAVEL_DISTANCE_SQ)
            distant.emplace_back(d2, &route);
    }

    // Nothing close: take the walk rather than the refusal.
    if (reachable.empty())
        reachable = std::move(distant);

    if (reachable.empty())
        return nullptr;

    size_t const pool = std::min(CANDIDATE_POOL, reachable.size());
    std::partial_sort(reachable.begin(), reachable.begin() + pool, reachable.end(),
                      [](auto const& a, auto const& b) { return a.first < b.first; });

    return reachable[urand(0, static_cast<uint32>(pool) - 1)].second;
}

GatherRouteMgr::Route const* GatherRouteMgr::PickRoute(Player* bot, uint32 zoneId) const
{
    std::vector<Route const*> candidates = GetRoutesFor(bot, zoneId);
    Route const* const* picked = RandomElement(candidates);
    return picked ? *picked : nullptr;
}
