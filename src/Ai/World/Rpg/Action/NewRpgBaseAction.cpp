/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include "GatherRouteMgr.h"
#include "CraftGoalMgr.h"
#include "ReagentSourceMgr.h"
#include "BotToolMgr.h"
#include "FishingAction.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridTerrainData.h"
#include "RandomUtils.h"
#include "IVMapMgr.h"
#include "NewRpgInfo.h"
#include "AttackersValue.h"
#include "DBCStores.h"
#include "MapMgr.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "BotAgendaMgr.h"
#include "BotHelpMgr.h"
#include "QuestBlacklistMgr.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "G3D/Vector2.h"
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <tuple>

QuestStatusData const* NewRpgBaseAction::GetQuestStatusData(uint32 questId) const
{
    QuestStatusMap const& statusMap = bot->getQuestStatusMap();
    auto itr = statusMap.find(questId);
    return itr != statusMap.end() ? &itr->second : nullptr;
}

/**
 * Snap a teleport destination onto solid ground, or refuse it.
 *
 * A teleport is the one movement that skips every check walking would have made. Both of ours took
 * a destination derived from map data -- a quest POI sample, a gather waypoint centroid -- and put
 * the bot there unconditionally. Those coordinates are not promises: a POI point has no Z at all
 * and a waypoint is an averaged centroid that can land inside a rock or in mid air. Arriving there
 * is how a bot ends up under the world and needs recovering.
 *
 * Same standard the safety manager recovers against, applied before the fact instead of after.
 */
bool NewRpgBaseAction::ResolveTeleportGround(Player* bot, WorldPosition& dest)
{
    Map* map = bot->FindMap();
    if (!map)
        return false;

    float const x = dest.GetPositionX();
    float const y = dest.GetPositionY();

    // Search downward from a little above the requested point, so a destination floating over a
    // ledge lands on the ledge rather than being rejected.
    float ground = map->GetHeight(bot->GetPhaseMask(), x, y, dest.GetPositionZ() + 5.0f, true);
    if (ground <= INVALID_HEIGHT || ground == VMAP_INVALID_HEIGHT_VALUE)
        ground = map->GetHeight(bot->GetPhaseMask(), x, y, MAX_HEIGHT, true);

    if (ground <= INVALID_HEIGHT || ground == VMAP_INVALID_HEIGHT_VALUE)
        return false;

    // Below the map's floor is outside the world, not underground -- the core's own distinction.
    if (ground < map->GetMinHeight(x, y))
        return false;

    float const water = map->GetWaterLevel(x, y);
    dest.Relocate(x, y, std::max(ground, water) + 0.5f);
    return true;
}

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    // A person is steering. Return before the stuck counters below are touched, not just before the
    // movement: suppressing the AI's movement is exactly what "no progress toward dest" looks like,
    // so without this the stuck timer runs down while somebody walks their own character around and
    // then teleports them to wherever the AI had been heading. Saying movement is in hand is also
    // true -- it is, by the person holding the keys.
    if (botAI->HumanIsDriving())
        return true;

    if (dest != botAI->rpgInfo.moveFarPos)
    {
        // clear stuck information if it's a new dest
        botAI->rpgInfo.SetMoveFarTo(dest);
    }

    // Waiting is not failing.
    //
    // This returned false, and every caller reads false as "could not move" and answers it with
    // MoveRandomNear -- a ten yard hop that clears the movement generator and destroys the walk
    // this function had just committed to. So a bot heading for a node two thousand yards away was
    // interrupted on the very next tick, every tick, and never arrived: 54 gather runs produced two
    // waypoint arrivals.
    //
    // The gate immediately below already returns true for the same situation once the bot is
    // visibly moving; the two disagreed only during the delay before the spline starts. Say the
    // same thing in both places -- movement is in hand, leave it alone.
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return true;
    }

    // Let previously committed movement finish before recomputing.
    //
    // MoveTo internally caps its stored delay at maxWaitForMove
    // (default 5s), but a long path (200+ yd routed around a
    // mountain) takes 30+ seconds to walk. After 5s
    // IsWaitingForLastMove returns false and MoveFarTo re-enters.
    // Without this gate, DoMovePoint would call mm->Clear() and
    // reissue MovePoint from the new bot position — and from a new
    // position mmap's partial-path endpoint often differs, so the
    // bot gets clobbered mid-walk and ends up oscillating (e.g.
    // cave entrance -> inside cave -> cave entrance -> mountain
    // base -> cave entrance...) around an unreachable destination.
    //
    // If the bot is still actively walking toward its last
    // committed point on the same map, just let the current spline
    // finish. The stuck counter below continues to track real
    // progress toward dest and triggers teleport recovery if the
    // committed paths genuinely aren't closing the gap.
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    // Fighting is not being stuck.
    //
    // A walk across a zone gets interrupted by whatever aggroes on the way. The bot stops, kills it,
    // and resumes -- and for the whole of that fight it makes no progress toward the destination, so
    // the counter below treats every interruption as evidence that the route is impossible. Three
    // fights along one route was enough to reach the threshold and teleport, which is exactly what
    // an operator watched happen: a few attempts to set off, a mob each time, and then the bot gave
    // up and blinked across the zone.
    //
    // The timer is reset rather than merely paused, because the fight moved the bot -- often
    // backwards, chasing something -- and the distance it had made before the fight is no longer the
    // baseline to judge the next attempt against.
    if (bot->IsInCombat())
    {
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
        botAI->rpgInfo.nearestMoveFarDis = bot->GetDistance(dest);
        return true;
    }

    // stuck check
    float disToDest = bot->GetDistance(dest);

    // Distance to the destination is not the only evidence of progress.
    //
    // A route around a lake, a ridge or a canyon spends whole legs moving sideways or away from the
    // destination, and this check only runs once the bot has reached the waypoint it was walking
    // to. So a bot walking a perfectly good path scored a stuck attempt at every waypoint whose leg
    // did not happen to close the straight line by five yards -- in open country, with nothing
    // wrong, on its way to arriving. An operator watched a character sit at "3/5 stuck" in an open
    // field, and at 5 the recovery below teleports, which is what a character blinking across the
    // zone looks like from the outside.
    //
    // Ground covered since progress was last accepted separates the two cases. A bot taking the
    // long way round is far from where it was judged; a bot oscillating around an obstacle returns
    // to roughly the same place however much it moves, so its distance from that snapshot stays
    // small. Measured against the snapshot rather than the previous tick for exactly that reason.
    constexpr float PROGRESS_TRAVEL_YARDS = 40.0f;

    // First time through for this destination there is nothing to compare against. Seed it here so
    // the very next evaluation has a baseline; without this the snapshot would only ever be written
    // by the progress branch below, and a bot that never closes the straight line -- the exact case
    // this exists for -- would never get one.
    if (!botAI->rpgInfo.stuckCheckPosValid)
    {
        botAI->rpgInfo.stuckCheckPos = WorldPosition(bot);
        botAI->rpgInfo.stuckCheckPosValid = true;
    }

    bool const travelled = botAI->rpgInfo.stuckCheckPosValid &&
                           bot->GetMapId() == botAI->rpgInfo.stuckCheckPos.GetMapId() &&
                           bot->GetExactDist(botAI->rpgInfo.stuckCheckPos.GetPositionX(),
                                             botAI->rpgInfo.stuckCheckPos.GetPositionY(),
                                             botAI->rpgInfo.stuckCheckPos.GetPositionZ()) > PROGRESS_TRAVEL_YARDS;

    // Require a meaningful improvement (5yd) to reset the stuck counter.
    // The old 1yd threshold was small enough that bots oscillating back
    // and forth around an obstacle would keep "making progress" forever
    // and never trigger the teleport recovery below.
    if (disToDest + 5.0f < botAI->rpgInfo.nearestMoveFarDis || travelled)
    {
        // Only ever lower the best-distance mark. Taking the long way round means the current
        // distance can be worse than the best already achieved, and raising the mark would hand the
        // bot a fresh five yards of slack every leg and defeat the oscillation check.
        botAI->rpgInfo.nearestMoveFarDis = std::min(botAI->rpgInfo.nearestMoveFarDis, disToDest);
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
        botAI->rpgInfo.stuckCheckPos = WorldPosition(bot);
        botAI->rpgInfo.stuckCheckPosValid = true;
    }
    else if (++botAI->rpgInfo.stuckAttempts >= 5 && GetMSTimeDiffToNow(botAI->rpgInfo.stuckTs) >= stuckTime)
    {
        // No meaningful progress toward dest for `stuckTime`: fall
        // back to teleporting directly so the bot can get on with
        // its RPG objective instead of oscillating indefinitely.
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;

        // P11.8 -- before teleporting, consider simply stepping off. A bot stuck on a platform is
        // the common case here, and a survivable drop solves it using the world rather than around
        // it. Only ahead of the teleport, never ahead of ordinary pathing: this is the last resort
        // before the more dangerous one.
        if (TryDeliberateDrop(dest))
            return true;

        const AreaTableEntry* entry = sAreaTableStore.LookupEntry(bot->GetZoneId());
        std::string zone_name = PlayerbotAI::GetLocalizedAreaName(entry);
        LOG_DEBUG(
            "playerbots",
            "[New RPG] Teleport {} from ({},{},{},{}) to ({},{},{},{}) as it stuck when moving far - Zone: {} ({})",
            bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId(), bot->GetZoneId(),
            zone_name);
        // Stuck recovery is one of exactly two places this layer teleports, and it is the more
        // dangerous: the destination was never walked to, so nothing has confirmed it is solid.
        WorldPosition landing = dest;
        if (!ResolveTeleportGround(bot, landing))
        {
            LOG_DEBUG("playerbots", "[Teleport] {} refused stuck recovery to ({:.0f},{:.0f},{:.0f}) on map {}: no "
                                    "ground there",
                      bot->GetName(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId());
            return false;
        }

        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        return bot->TeleportTo(landing);
    }

    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
        return MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false, false,
                      false, true);
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // Primary strategy: ask mmap for a route to the TRUE destination.
    // If mmap can reach it directly (PATHFIND_NORMAL) or partially
    // (PATHFIND_INCOMPLETE — destinations beyond the smooth-path cap
    // of ~296 yards, or where local geometry blocks the final step),
    // walk to the furthest reachable waypoint mmap computed. This
    // lets bots follow the real route around obstacles (mountains,
    // cave walls, cliffs) instead of trying to cut straight through.
    // The spline system walks the whole returned path smoothly, so
    // subsequent ticks early-out via IsWaitingForLastMove and no
    // further PathGenerator calls fire until the bot arrives.
    {
        PathGenerator path(bot);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));
        if (canReach)
        {
            const G3D::Vector3& endPos = path.GetActualEndPosition();
            // Only commit if the mmap endpoint actually makes progress
            // toward the destination. For pathological INCOMPLETE
            // results (e.g. disconnected polys that still report
            // INCOMPLETE) the endpoint can land right under the bot;
            // fall through to cone sampling in that case.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            if (endDistToDest + 5.0f < disToDest)
            {
                return MoveTo(bot->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, true);
            }
        }
    }

    // Fallback: mmap couldn't route to the destination. Sample the
    // forward cone for a reachable stepping stone so the bot keeps
    // moving and can try again from a new vantage point. Cap at 2
    // samples — we already spent one PathGenerator call above and at
    // 3000 bots every extra CalculatePath matters.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx, ry, rz;
    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            const G3D::Vector3& endPos = path.GetActualEndPosition();
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);
    }
    return false;
}

bool NewRpgBaseAction::MoveWorldObjectTo(ObjectGuid guid, float distance)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    WorldObject* object = botAI->GetWorldObject(guid);
    if (!object)
        return false;
    float x = object->GetPositionX();
    float y = object->GetPositionY();
    float z = object->GetPositionZ();
    float mapId = object->GetMapId();
    float angle = 0.f;

    if (!object->ToUnit() || !object->ToUnit()->isMoving())
        angle = object->GetAngle(bot) + (M_PI * irand(-25, 25) / 100.0);  // Closest 45 degrees towards the target
    else
        angle = object->GetOrientation() +
                (M_PI * irand(-25, 25) / 100.0);  // 45 degrees infront of target (leading it's movement)

    float rnd = rand_norm();
    x += cos(angle) * distance * rnd;
    y += sin(angle) * distance * rnd;
    if (!object->GetMap()->CheckCollisionAndGetValidCoords(object, object->GetPositionX(), object->GetPositionY(),
                                                           object->GetPositionZ(), x, y, z))
    {
        x = object->GetPositionX();
        y = object->GetPositionY();
        z = object->GetPositionZ();
    }
    return MoveTo(mapId, x, y, z, false, false, false, true);
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject*)
{
    if (IsWaitingForLastMove(priority))
        return false;

    Map* map = bot->GetMap();
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    // Previously: attempts = 1. A single random sample often landed in
    // water / blocked geometry / unreachable poly, the function returned
    // false, and the caller had no fallback — bot stood still. Retry a
    // few times with a fresh distance each loop so a bad roll doesn't
    // lock the bot in place.
    //
    // Capped at 3, down from 8. Every attempt is a full PathGenerator
    // (Detour) query, and this is called from four different NewRpg
    // actions plus the MoveFarTo fallbacks, so the worst case was ~11
    // Detour queries per bot per tick from the RPG system alone. Three
    // samples still clear a bad roll; the caller retries next tick.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float dx = x + distance * cos(angle);
        float dy = y + distance * sin(angle);
        float dz = z;

        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        bool canReach = !(type & (~typeOk));

        if (!canReach)
            continue;

        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), dx, dy, dz, false, false, false, true, priority);
        if (moved)
            return true;
    }

    return false;
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    return true;
}

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    bot->PrepareQuestMenu(guid);
    const QuestMenu& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        const QuestMenuItem& item = menu.GetItem(idx);
        const Quest* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        const QuestStatus& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest))
        {
            AcceptQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->rpgStatistic.questAccepted++;
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            TurnInQuest(quest, guid);
            // A completion is the strongest possible evidence that this quest is workable, so it
            // clears any failure verdict other bots have built up against it.
            sQuestBlacklistMgr.ReportSuccess(quest->GetQuestId());

            // Whatever was blocking this bot is resolved; stop sending people.
            sBotHelpMgr.ClearRequest(bot->GetGUID());
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->rpgStatistic.questRewarded++;
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

bool NewRpgBaseAction::CanInteractWithQuestGiver(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that removes the distance check and keeps all other checks
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT: // Player::GetNPCIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            // unit checks
            if (!guid)
                return false;

            if (!bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
                return false;

            if (bot->IsInFlight())
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            // Deathstate checks
            if (!bot->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_VISIBLE_TO_GHOSTS))
                return false;

            // alive or spirit healer
            if (!creature->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_INTERACT_WHILE_DEAD))
                return false;

            // appropriate npc type
            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                return false;

            // not allow interaction under control, but allow with own pets
            if (creature->GetCharmerGUID())
                return false;

            // xinef: perform better check
            if (creature->GetReactionTo(bot) <= REP_UNFRIENDLY)
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT: // Player::GetGameObjectIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
                {
                    // Players cannot interact with gameobjects that use the "Point" icon
                    if (go->GetGOInfo()->IconName == "Point")
                        return false;

                    return true;
                }
            }

            return false;
        }
        // unused for now
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    return true;
}

bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                float score = calc.CalculateItem(quest->RewardChoiceItemId[i]);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::IsQuestWorthDoing(Quest const* quest)
{
    bool isLowLevelQuest =
        bot->GetLevel() > (bot->GetQuestLevel(quest) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));

    if (isLowLevelQuest)
        return false;

    if (quest->IsRepeatable())
        return false;

    if (quest->IsSeasonal())
        return false;

    // Escorts are not attempted at all -- see QuestBlacklistMgr::IsEscortQuest. This is a hard gate
    // rather than a score penalty because there is no level of desperation at which a bot should
    // pick one up.
    if (sQuestBlacklistMgr.IsEscortQuest(quest->GetQuestId()))
        return false;

    return true;
}

bool NewRpgBaseAction::IsQuestCapableDoing(Quest const* quest)
{
    bool highLevelQuest = bot->GetLevel() + 3 < bot->GetQuestLevel(quest);
    if (highLevelQuest)
        return false;

    // Quest::Type is QuestInfoID -- column 5 of the quest_template SELECT, not the QuestType column
    // that shares its name. Measured across 9464 quests: 7823 are ordinary (0), 530 dungeon (81),
    // 380 group (1), 362 raid (62), 280 PvP (41), the rest heroic and raid variants.
    uint32 const info = quest->GetType();

    bool const grouped = bot->GetGroup() != nullptr;

    // Instance and PvP quests are allowed. An earlier version of this excluded them on the
    // assumption that bots had no machinery for the content, which was simply false: the module
    // ships boss strategies for ICC, Naxx, Ulduar, Karazhan, Black Temple, Molten Core, BWL, Hyjal,
    // Gruul, Magtheridon and more, a Dungeon strategy tree, and LFG join actions. Bots already fill
    // dungeon and battleground queues for human players; the operator wants them to do it on their
    // own account too.
    //
    // What genuinely needs care is *concurrency*, not eligibility -- a thousand bots deciding to
    // start an instance at once would take the realm down. That throttle belongs to the autonomous
    // group-content work (Phase 13) and gates the running of instances, not the holding of quests.
    //
    // Note the ordering dependency: until Phase 13 lands, instance quests will be accepted and then
    // mostly fail for want of a way in, which will push them through QuestBlacklistMgr. That is
    // acceptable and self-correcting -- the blacklist forgets a quest the moment any bot completes
    // it -- but it is the reason to expect instance quests to look unproductive until then.
    (void)info;

    // Suggested party size is respected only while ungrouped. In a group the party is the answer to
    // the suggestion, which is the whole point of P7.29 bringing one together.
    if (!grouped && quest->GetSuggestedPlayers() >= 2)
        return false;

    return true;
}

void NewRpgBaseAction::DropQuest(uint16 slot, uint32 questId, Quest const* quest)
{
    LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);

    WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
    packet << static_cast<uint8>(slot);
    WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
    removeQuest.Read();
    bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);

    if (quest && botAI->GetMaster())
        botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "new_rpg_quest_dropped",
            "Quest dropped %quest",
            {{"%quest", ChatHelper::FormatQuest(quest)}}));

    botAI->rpgStatistic.questDropped++;
}

float NewRpgBaseAction::ScoreQuestObjectiveShape(uint32 questId, Quest const* quest)
{
    if (!quest)
        return 0.0f;

    float score = 0.0f;

    bool hasKill = false;
    bool hasCollect = false;

    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredNpcOrGoCount[i])
            hasKill = true;

    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemCount[i])
            hasCollect = true;

    if (hasKill)
        score += 40.0f;
    if (hasCollect)
        score += 40.0f;

    // Neither: completion hangs on talking to somebody, reaching a trigger, or a script firing.
    if (!hasKill && !hasCollect)
        score -= 60.0f;

    // A bot that fails a timed quest has spent the trip and the quest slot for nothing. There is
    // no QUEST_FLAGS_TIMED despite the name appearing in some documentation -- flag 0x1 is
    // STAY_ALIVE, which is a different (also bot-hostile) thing. Timed quests carry TimeAllowed.
    if (quest->GetTimeAllowed() > 0)
        score -= 40.0f;

    // Fails if the bot dies. Bots die: to adds, to falling, to a patrol wandering into a fight.
    if (quest->HasFlag(QUEST_FLAGS_STAY_ALIVE))
        score -= 25.0f;

    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
        score -= 30.0f;

    if (sQuestBlacklistMgr.IsEscortQuest(questId))
        score -= 100.0f;

    // P7.2 -- chain position. A quest that opens three more is worth more than one that opens none,
    // because following a chain is how a bot ends up working through a zone's storyline rather than
    // doing scattered one-offs in whatever order it stumbled across them. Capped, so a hub quest
    // that unlocks a dozen followers does not outrank progress on something half-finished.
    if (uint32 const unlocks = sQuestBlacklistMgr.GetUnlockCount(questId))
        score += std::min(unlocks, 3u) * 15.0f;

    // Being mid-chain is itself worth something: the bot has already done the prerequisite, so this
    // quest is where its previous work pays off.
    if (quest->GetPrevQuestId() != 0)
        score += 10.0f;

    return score;
}

float NewRpgBaseAction::ScoreQuestForKeeping(uint32 questId, Quest const* quest)
{
    // Template missing from quest_template - nothing can ever be done with it.
    if (!quest)
        return -10000.0f;

    QuestStatus status = bot->GetQuestStatus(questId);

    if (status == QUEST_STATUS_FAILED)
        return -1000.0f;

    // A completed quest is one interaction away from XP and a reward. Never worth shedding.
    if (status == QUEST_STATUS_COMPLETE)
        return 1000.0f;

    float score = 0.0f;

    if (!IsQuestWorthDoing(quest))
        score -= 500.0f;

    if (!IsQuestCapableDoing(quest))
        score -= 400.0f;

    // Objective progress. Dropping a quest the bot has already partly done throws that work away,
    // which is most of why the old cascade was so wasteful.
    if (QuestStatusData const* statusData = GetQuestStatusData(questId))
    {
        float required = 0.0f;
        float done = 0.0f;

        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            if (uint32 need = quest->RequiredNpcOrGoCount[i])
            {
                required += need;
                done += std::min<uint32>(statusData->CreatureOrGOCount[i], need);
            }
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            if (uint32 need = quest->RequiredItemCount[i])
            {
                required += need;
                done += std::min<uint32>(statusData->ItemCount[i], need);
            }
        }

        if (required > 0.0f)
            score += 300.0f * (done / required);
    }

    // P7.6 -- an unfinished quest in the bot's current zone is worth holding onto, because leaving
    // a zone with work still in it is how bots end up with a log full of half-done quests spread
    // across a continent. This reinforces the existing same-zone preference rather than replacing
    // it, and is deliberately smaller than the progress term: a half-finished quest elsewhere still
    // beats an untouched one here.
    if (bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
    {
        int32 const zoneOrSortHere = quest->GetZoneOrSort();
        if (zoneOrSortHere > 0 && static_cast<uint32>(zoneOrSortHere) == bot->GetZoneId())
            score += 40.0f;
    }

    // Prefer quests the bot can act on where it currently is. This used to be a hard drop rule,
    // which meant a bot discarded its in-progress quests every time it crossed a zone border (or
    // got teleported by the MoveFarTo stuck recovery). It is a preference now, not a kill switch.
    int32 zoneOrSort = quest->GetZoneOrSort();
    if (zoneOrSort > 0 && static_cast<uint32>(zoneOrSort) == bot->GetZoneId())
        score += 150.0f;
    else if (zoneOrSort < 0)
        score -= 50.0f;  // a class/profession/seasonal sort bucket rather than a real zone

    // Without POI data the RPG system has no way to navigate the quest at all.
    if (!sObjectMgr->GetQuestPOIVector(questId))
        score -= 200.0f;

    // P7.4 corrects a P1.1 decision. The original penalised level mismatch symmetrically --
    // abs(levelGap) * 5 -- which treats a green quest and a red quest as equally undesirable. They
    // are not remotely equivalent.
    //
    // A green quest is *perishable*: it still grants experience now and will grant none once it
    // turns grey, so its value decays with every level the bot gains. A red quest is merely
    // difficult, and gets easier for free by waiting. Penalising the perishable one as hard as the
    // renewable one is how bots end up carrying red quests they cannot do while green quests expire
    // unfinished in the same log.
    int32 const levelGap = static_cast<int32>(bot->GetLevel()) - static_cast<int32>(bot->GetQuestLevel(quest));

    if (levelGap > 0)
    {
        // Bot outlevels the quest. Mild, and only once it is close to grey -- until then this is
        // easy experience the bot should be finishing, not shedding.
        int32 const greyLevel = static_cast<int32>(bot->GetQuestLevel(quest)) +
                                static_cast<int32>(sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));
        bool const nearlyGrey = static_cast<int32>(bot->GetLevel()) >= greyLevel - 2;

        score -= nearlyGrey ? levelGap * 2.0f : 0.0f;

        // Actively prefer finishing it while it still pays.
        if (!nearlyGrey)
            score += 60.0f;
    }
    else
    {
        // Quest is above the bot. Harder, and the penalty grows with the gap because the bot may
        // simply be unable to survive it -- but it keeps its value while the bot levels.
        score -= std::abs(levelGap) * 8.0f;
    }

    // P7.5 -- never select a trivial quest for dropping. A quest the bot has outlevelled is usually
    // one interaction from a turn-in, so completing it is cheaper than dropping it and re-acquiring
    // something else. This sits below the COMPLETE check above, which already protects finished
    // quests outright.
    if (bot->GetLevel() > bot->GetQuestLevel(quest) + 5 && bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
        score += 120.0f;

    // Objectives the bot can actually drive. Weighted well below progress and completion: a
    // half-finished awkward quest is still worth more than a pristine convenient one.
    score += ScoreQuestObjectiveShape(questId, quest);

    // Already written off after repeatedly failing to make progress on it.
    if (sQuestBlacklistMgr.IsBlacklisted(questId))
        score -= 600.0f;

    return score;
}

bool NewRpgBaseAction::TeleportToDistantTurnIn(uint32 questId, WorldPosition const& pos)
{
    if (!sPlayerbotAIConfig.questTurnInTeleport)
        return false;

    // Grouped bots walk. A party member that teleports away mid-quest is the single most
    // bot-looking thing a bot can do.
    if (bot->GetGroup())
        return false;

    if (bot->IsInCombat() || bot->isDead() || bot->IsBeingTeleported())
        return false;

    if (pos == WorldPosition() || pos.GetMapId() != bot->GetMapId())
        return false;

    float const distance = bot->GetExactDist(pos);
    if (distance < sPlayerbotAIConfig.questTurnInTeleportDistance)
        return false;

    // The second and last teleport in this layer. A quest giver's recorded position is usually
    // sound, but it comes from the same POI data as everything else and is not worth trusting
    // blind.
    WorldPosition landing = pos;
    if (!ResolveTeleportGround(bot, landing))
    {
        LOG_DEBUG("playerbots", "[Teleport] {} refused quest turn-in jump for quest {}: no ground at the destination",
                  bot->GetName(), questId);
        return false;
    }

    LOG_DEBUG("playerbots", "[QuestTeleport] {} skipping {:.0f} yards to turn in quest {}", bot->GetName(),
              distance, questId);

    bot->TeleportTo(landing.GetMapId(), landing.GetPositionX(), landing.GetPositionY(), landing.GetPositionZ(),
                    bot->GetOrientation());
    return true;
}

uint32 NewRpgBaseAction::AutoCompleteTrivialQuests()
{
    if (!sPlayerbotAIConfig.autoCompleteTrivialQuests)
        return 0;

    uint32 completed = 0;
    uint32 const greyDiff = sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        // Grey only. A quest that still pays experience must be earned.
        if (bot->GetLevel() <= bot->GetQuestLevel(quest) + greyDiff)
            continue;

        QuestStatus const status = bot->GetQuestStatus(questId);
        if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
            continue;

        // Only advance a quest whose objectives are actually met.
        //
        // This used to mark the quest complete unconditionally and leave CanRewardQuest below to
        // sort it out. CanRewardQuest re-derives completion from the objectives, so for any quest
        // the bot had not really done it refused -- and the `continue` walked away leaving the
        // quest stored QUEST_STATUS_COMPLETE with its objectives unmet.
        //
        // That record is a permanent stall, not a missed reward. The questgiver marker and the
        // bot's turn-in planning read the stored status, so the giver wears a yellow '?' and the
        // bot walks to it; the hand-in re-validates and refuses; the stored status still says
        // complete, so it goes straight back. It also survived logout, which is why the same two
        // quests kept reappearing after being repaired at login -- the repair was correct and this
        // ran again minutes later.
        if (status == QUEST_STATUS_INCOMPLETE)
        {
            if (!bot->CanCompleteQuest(questId))
                continue;

            bot->CompleteQuest(questId);
        }

        // Pick a reward the bot would actually want, rather than always taking the first option.
        // ItemUsageValue already knows this bot's class, spec and what it is wearing, so the choice
        // is consistent with how it judges the same item from any other source.
        uint32 choice = 0;
        if (uint32 const choices = quest->GetRewChoiceItemsCount())
        {
            float best = -1.0f;
            for (uint32 i = 0; i < choices; ++i)
            {
                uint32 const itemId = quest->RewardChoiceItemId[i];
                if (!itemId)
                    continue;

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto)
                    continue;

                ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", itemId);

                // An upgrade beats everything; otherwise take whatever is worth most, since it is
                // going to the auction house or a vendor either way.
                float const score = (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                                        ? 1000000.0f + proto->SellPrice
                                        : static_cast<float>(proto->SellPrice);

                if (score > best)
                {
                    best = score;
                    choice = i;
                }
            }
        }

        if (!bot->CanRewardQuest(quest, choice, false))
        {
            // Put back what we changed. The guard above means the objectives are met, but this can
            // still refuse for reasons of its own -- no bag space for the reward item is the common
            // one -- and leaving our own mark behind would recreate exactly the corruption this
            // function is now careful not to produce.
            if (status == QUEST_STATUS_INCOMPLETE)
                bot->IncompleteQuest(questId);

            continue;
        }

        // The bot itself stands in for the quest giver. Skipping the walk back is the same
        // sanctioned abstraction as posting to the auction house without an auctioneer: the reward,
        // the reputation and the log slot all move exactly as they would have.
        bot->RewardQuest(quest, choice, bot, false);
        ++completed;

        LOG_DEBUG("playerbots", "[Quest] {} auto-completed trivial quest {} (quest level {}, bot level {})",
                  bot->GetName(), questId, bot->GetQuestLevel(quest), bot->GetLevel());
    }

    return completed;
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    // Free slots by finishing what is already finished, before considering dropping anything.
    if (AutoCompleteTrivialQuests())
        return true;

    uint32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        if (!bot->GetQuestSlotQuestId(i))
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    // Score every logged quest and shed only the worst few.
    //
    // This replaced a three-pass cascade whose final pass was labelled "clear quests log" and
    // dropped EVERY remaining quest unconditionally. Combined with its second pass, which dropped
    // any quest whose zone did not match the bot's current zone, a bot walking across a zone
    // border could lose its entire quest log. That was the single largest cause of the
    // accept/abandon churn visible in NewRpgStatistic.
    struct ScoredQuest
    {
        uint16 slot;
        uint32 questId;
        Quest const* quest;
        float score;
    };

    std::vector<ScoredQuest> scored;
    scored.reserve(MAX_QUEST_LOG_SIZE);

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        scored.push_back({i, questId, quest, ScoreQuestForKeeping(questId, quest)});
    }

    if (scored.empty())
        return false;

    std::sort(scored.begin(), scored.end(),
              [](ScoredQuest const& a, ScoredQuest const& b) { return a.score < b.score; });

    uint32 const maxDrops = std::max<uint32>(1, sPlayerbotAIConfig.questMaxDropsPerPass);
    uint32 dropped = 0;

    for (ScoredQuest const& entry : scored)
    {
        if (dropped >= maxDrops)
            break;

        // Never shed a quest that is ready to hand in, even if the whole log is full of them -
        // in that case the right answer is to go turn one in, not to throw a reward away.
        if (entry.quest && bot->GetQuestStatus(entry.questId) == QUEST_STATUS_COMPLETE)
            continue;

        // Clearing a slot does not compact the log, so the slots collected above stay valid.
        DropQuest(entry.slot, entry.questId, entry.quest);
        dropped++;
    }

    return dropped > 0;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    // Throttled per bot. Nothing here needs tick resolution: questgivers do not move, and a bot
    // that walks past one will still see it on the next pass a couple of seconds later.
    if (botAI->lastQuestGiverSearch && GetMSTimeDiffToNow(botAI->lastQuestGiverSearch) < questGiverSearchInterval)
        return false;

    botAI->lastQuestGiverSearch = getMSTime();

    if (!botAI->lastQuestLogOrganize || GetMSTimeDiffToNow(botAI->lastQuestLogOrganize) >= questLogOrganizeInterval)
    {
        botAI->lastQuestLogOrganize = getMSTime();
        OrganizeQuestLog();
    }

    if (ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract(true, 80.0f))
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
        if (bot->CanInteractWithQuestGiver(object))
        {
            InteractWithNpcOrGameObjectForQuest(npcOrGo);
            ForceToWait(5000);
            return true;
        }
        return MoveWorldObjectTo(npcOrGo);
    }
    return false;
}

/// Returns the nearest object in `candidates` this bot can accept or hand in a quest at.
///
/// Both `possible new rpg targets` and `possible new rpg game objects` are produced already sorted
/// by distance - see PossibleNewRpgTargetsValue::Calculate and
/// PossibleNewRpgGameObjectsValue::Calculate - so the first match IS the nearest match and the scan
/// can stop there. That early exit is not just an optimisation: HasQuestToAcceptOrReward calls
/// Player::PrepareQuestMenu, which does DB-backed relation lookups and mutates PlayerTalkClass, so
/// scanning a whole city's worth of candidates every tick is not affordable.
///
/// If either value ever stops sorting by distance, this function silently degrades to "first
/// acceptable" rather than "nearest acceptable".
WorldObject* NewRpgBaseAction::FindNearestQuestGiver(GuidVector const& candidates, float distanceLimit)
{
    for (ObjectGuid const& guid : candidates)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

        if (!object || !object->IsInWorld())
            continue;

        if (distanceLimit && bot->GetDistance(object) > distanceLimit)
            continue;

        if (CanInteractWithQuestGiver(object) && HasQuestToAcceptOrReward(object))
            return object;
    }

    return nullptr;
}

ObjectGuid NewRpgBaseAction::ChooseNpcOrGameObjectToInteract(bool questgiverOnly, float distanceLimit)
{
    GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
    GuidVector possibleGameObjects = AI_VALUE(GuidVector, "possible new rpg game objects");

    if (possibleTargets.empty() && possibleGameObjects.empty())
        return ObjectGuid();

    WorldObject* nearestObject = FindNearestQuestGiver(possibleTargets, distanceLimit);

    if (WorldObject* nearestGameObject = FindNearestQuestGiver(possibleGameObjects, distanceLimit))
    {
        if (!nearestObject || bot->GetExactDist(nearestGameObject) < bot->GetExactDist(nearestObject))
            nearestObject = nearestGameObject;
    }

    if (nearestObject)
        return nearestObject->GetGUID();

    // No questgiver to accept or reward
    if (questgiverOnly)
        return ObjectGuid();

    if (possibleTargets.empty())
        return ObjectGuid();

    ObjectGuid guid = *RandomElement(possibleTargets);
    WorldObject* object = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
    if (!object)
        object = ObjectAccessor::GetGameObject(*bot, guid);

    if (object && object->IsInWorld())
    {
        return object->GetGUID();
    }
    return ObjectGuid();
}

bool NewRpgBaseAction::HasQuestToAcceptOrReward(WorldObject* object)
{
    ObjectGuid guid = object->GetGUID();
    bot->PrepareQuestMenu(guid);
    const QuestMenu& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return false;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        const QuestMenuItem& item = menu.GetItem(idx);
        const Quest* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;
        const QuestStatus& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            return true;
        }
    }
    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        const QuestMenuItem& item = menu.GetItem(idx);
        const Quest* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        const QuestStatus& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest))
        {
            return true;
        }
    }
    return false;
}

/// Standard ray-casting point-in-polygon test.
static bool IsPointInPolygon(std::vector<QuestPOIPoint> const& poly, float x, float y)
{
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        float xi = static_cast<float>(poly[i].x);
        float yi = static_cast<float>(poly[i].y);
        float xj = static_cast<float>(poly[j].x);
        float yj = static_cast<float>(poly[j].y);

        // The half-open y test guarantees yi != yj by the time we divide.
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            inside = !inside;
    }
    return inside;
}

/// Produce up to `maxSamples` points that lie INSIDE a quest POI polygon.
///
/// The previous implementation took a random-weighted average of every vertex. For any POI that
/// is not roughly convex and centre-filled - a ring around a lake, a valley rim, two lobes joined
/// by a corridor - that average lands somewhere the objective is not. The bot then walked there,
/// found nothing, and blacklisted the quest. Rejection-sampling the polygon interior instead keeps
/// every candidate somewhere the quest designer actually marked.
static void SampleQuestPoiPoints(std::vector<QuestPOIPoint> const& points, std::vector<G3D::Vector2>& out,
                                 uint32 maxSamples)
{
    if (points.empty() || !maxSamples)
        return;

    if (points.size() == 1)
    {
        out.emplace_back(static_cast<float>(points[0].x), static_cast<float>(points[0].y));
        return;
    }

    float minX = static_cast<float>(points[0].x);
    float maxX = minX;
    float minY = static_cast<float>(points[0].y);
    float maxY = minY;
    float centroidX = 0.0f;
    float centroidY = 0.0f;

    for (QuestPOIPoint const& point : points)
    {
        float px = static_cast<float>(point.x);
        float py = static_cast<float>(point.y);
        minX = std::min(minX, px);
        maxX = std::max(maxX, px);
        minY = std::min(minY, py);
        maxY = std::max(maxY, py);
        centroidX += px;
        centroidY += py;
    }
    centroidX /= static_cast<float>(points.size());
    centroidY /= static_cast<float>(points.size());

    // Two points describe a line, not an area - offer both ends and the midpoint.
    if (points.size() == 2)
    {
        out.emplace_back(centroidX, centroidY);
        if (maxSamples > 1)
            out.emplace_back(static_cast<float>(points[0].x), static_cast<float>(points[0].y));
        if (maxSamples > 2)
            out.emplace_back(static_cast<float>(points[1].x), static_cast<float>(points[1].y));
        return;
    }

    uint32 const maxTries = maxSamples * 8;
    for (uint32 tries = 0; tries < maxTries && out.size() < maxSamples; ++tries)
    {
        float x = frand(minX, maxX);
        float y = frand(minY, maxY);
        if (IsPointInPolygon(points, x, y))
            out.emplace_back(x, y);
    }

    if (!out.empty())
        return;

    // Degenerate or extremely thin polygon: rejection sampling can miss it entirely. Fall back to
    // the vertices, pulled a fifth of the way toward the centroid so we sit just inside the
    // outline rather than exactly on the boundary.
    for (size_t i = 0; i < points.size() && out.size() < maxSamples; ++i)
    {
        float x = static_cast<float>(points[i].x);
        float y = static_cast<float>(points[i].y);
        out.emplace_back(x + (centroidX - x) * 0.2f, y + (centroidY - y) * 0.2f);
    }
}

void NewRpgBaseAction::AddPoiCandidates(QuestPOI const& qPoi, std::vector<POIInfo>& poiInfo)
{
    std::vector<G3D::Vector2> samples;
    SampleQuestPoiPoints(qPoi.points, samples, poiSamplesPerArea);

    Map* map = bot->GetMap();
    // A candidate that landed in open water is held back rather than dropped: a POI polygon
    // overlapping a lake should prefer the shore, but some objectives genuinely are underwater and
    // discarding those outright would make the quest look unreachable.
    bool haveSubmerged = false;
    POIInfo submerged{};

    for (G3D::Vector2 const& sample : samples)
    {
        float dx = sample.x;
        float dy = sample.y;

        if (bot->GetDistance2d(dx, dy) >= 1500.0f)
            continue;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(map->GetHeight(dx, dy, MAX_HEIGHT), map->GetWaterLevel(dx, dy));

        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            continue;

        if (bot->GetZoneId() != map->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
        {
            if (!haveSubmerged)
            {
                submerged = {{dx, dy}, qPoi.ObjectiveIndex};
                haveSubmerged = true;
            }
            continue;
        }

        // One good candidate per POI is enough, and this return is load-bearing.
        //
        // Validating all `poiSamplesPerArea` samples made this function roughly 5x the cost of
        // the centroid it replaced (4 samples x GetHeight/GetWaterLevel/GetZoneId/IsInWater,
        // versus one sample x three queries). That lands on a genuinely hot path:
        // RandomChangeStatus's DO_QUEST branch walks all 25 quest slots with no early exit and
        // CheckRpgStatusAvailable walks them again, so this can run ~50 times per idle bot per
        // tick. Measured at 200 bots it cost ~14% mean tick time and ~19% at p95.
        //
        // Candidate diversity does not come from validating four at once. Sampling is random on
        // every call and the caller retries with a fresh sample set when a POI proves
        // unproductive (DoQuest::poiAttempts), so diversity comes from retries. Callers that want
        // several candidates still get one per POI, and quests routinely have several POIs.
        poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
        return;
    }

    if (haveSubmerged)
        poiInfo.push_back(submerged);
}

namespace
{
/// entry -> every place that entry is spawned, indexed once on first use.
using SpawnIndex = std::unordered_map<uint32, std::vector<std::tuple<uint16, float, float, float>>>;

SpawnIndex g_creatureSpawns;
SpawnIndex g_objectSpawns;

// Quest item -> the chest entries that can contain it.
std::unordered_map<uint32, std::vector<uint32>> g_itemSourceObjects;

std::once_flag g_spawnIndexOnce;

void BuildSpawnIndex()
{
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        // id2 and id3 are alternates from creature_multispawn; a quest target can be any of them.
        for (uint32 entry : {data.id, data.id2, data.id3})
            if (entry)
                g_creatureSpawns[entry].emplace_back(data.mapid, data.posX, data.posY, data.posZ);
    }

    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        if (data.id)
            g_objectSpawns[data.id].emplace_back(data.mapid, data.posX, data.posY, data.posZ);

    // An item objective names no creature and no object, so a bot carrying one has nothing to walk
    // to and falls back to the quest's map pin -- which carries X and Y but no Z at all. For
    // anything not lying on open ground that pin is unusable: the Bladefist Bay toolboxes behind
    // "From The Wreckage...." sit on the seabed between -0.7 and -20.3, and a bot aiming at the
    // surface coordinate above them never descends. Resolving the required item back to the chests
    // that contain it recovers a real height to aim at.
    //
    // Every chest drop, unfiltered. An earlier version narrowed this to items some quest actually
    // asks for, with `IN (SELECT ... UNION ...)` over the six RequiredItemId columns. MySQL
    // re-evaluated that subquery per row and the statement ran for over ten minutes -- and because
    // this function is called under std::call_once from the world thread, it took the entire realm
    // with it: no bot updates, no console output, and clients stuck at "Connected".
    //
    // The filter was never worth having. Unfiltered this is a plain two-table join returning 33,449
    // pairs in 0.05s, and the map it builds is small enough that narrowing it saved nothing.
    //
    // The general rule this cost us: anything queried here runs on the world thread with the realm
    // waiting on it. Keep it to something that cannot become slow.
    QueryResult itemSources = WorldDatabase.Query(
        "SELECT DISTINCT glt.Item, gt.entry "
        "FROM gameobject_loot_template glt "
        "JOIN gameobject_template gt ON gt.Data1 = glt.Entry AND gt.type = 3");

    if (itemSources)
    {
        do
        {
            Field* fields = itemSources->Fetch();
            g_itemSourceObjects[fields[0].Get<uint32>()].push_back(fields[1].Get<uint32>());
        } while (itemSources->NextRow());
    }

    LOG_INFO("server.loading",
             ">> Indexed spawns for {} creatures and {} objects, and {} quest items to their source objects, "
             "for quest routing",
             g_creatureSpawns.size(), g_objectSpawns.size(), g_itemSourceObjects.size());
}
}  // namespace

bool NewRpgBaseAction::AddSpawnCandidates(Quest const* quest, int32 objectiveIdx, std::vector<POIInfo>& poiInfo)
{
    if (objectiveIdx < 0)
        return false;

    std::call_once(g_spawnIndexOnce, BuildSpawnIndex);

    // Every spawn list this objective could be satisfied at. Usually one, but an item that drops
    // from several kinds of chest has one list per chest.
    std::vector<std::vector<std::tuple<uint16, float, float, float>> const*> sources;

    if (objectiveIdx < QUEST_OBJECTIVES_COUNT)
    {
        int32 const npcOrGo = quest->RequiredNpcOrGo[objectiveIdx];
        if (!npcOrGo)
            return false;

        // A negative RequiredNpcOrGo means a game object, positive means a creature.
        SpawnIndex const& index = npcOrGo > 0 ? g_creatureSpawns : g_objectSpawns;
        auto itr = index.find(uint32(std::abs(npcOrGo)));
        if (itr == index.end())
            return false;

        sources.push_back(&itr->second);
    }
    else
    {
        // Item objective: aim at whatever holds the item. Objective indices past the creature slots
        // are item slots, which is the convention GetQuestPOIPosAndObjectiveIdx already uses.
        int32 const itemSlot = objectiveIdx - QUEST_OBJECTIVES_COUNT;
        if (itemSlot >= QUEST_ITEM_OBJECTIVES_COUNT)
            return false;

        uint32 const itemId = quest->RequiredItemId[itemSlot];
        if (!itemId)
            return false;

        auto sourceItr = g_itemSourceObjects.find(itemId);
        if (sourceItr == g_itemSourceObjects.end())
            return false;

        for (uint32 objectEntry : sourceItr->second)
            if (auto itr = g_objectSpawns.find(objectEntry); itr != g_objectSpawns.end())
                sources.push_back(&itr->second);

        if (sources.empty())
            return false;
    }

    size_t const before = poiInfo.size();

    for (auto const* spawnList : sources)
    {
        for (auto const& [mapId, x, y, z] : *spawnList)
        {
            if (mapId != bot->GetMapId())
                continue;

            // Same reach the pin sampler uses, so this does not send a bot across a continent.
            if (bot->GetDistance2d(x, y) >= 1500.0f)
                continue;

            POIInfo info;
            info.pos = G3D::Vector2(x, y);
            info.objectiveIdx = objectiveIdx;
            info.z = z;
            info.hasZ = true;
            poiInfo.push_back(info);
        }
    }

    return poiInfo.size() > before;
}

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    const QuestPOIVector* poiVector = sObjectMgr->GetQuestPOIVector(questId);
    if (!poiVector)
    {
        return false;
    }

    QuestStatusData const* statusData = GetQuestStatusData(questId);
    if (!statusData)
        return false;

    QuestStatusData const& q_status = *statusData;

    if (toComplete && q_status.Status == QUEST_STATUS_COMPLETE)
    {
        for (const QuestPOI& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId())
                continue;

            // not the poi pos to reward quest
            if (qPoi.ObjectiveIndex != -1)
                continue;

            if (qPoi.points.empty())
                continue;

            AddPoiCandidates(qPoi, poiInfo);
        }

        if (poiInfo.empty())
            return false;

        return true;
    }

    if (q_status.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    // Get incomplete quest objective index
    std::vector<int32> incompleteObjectiveIdx;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        int32 npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;

        if (q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            incompleteObjectiveIdx.push_back(i);
    }
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
    {
        uint32 itemId = quest->RequiredItemId[i];
        if (!itemId)
            continue;

        if (q_status.ItemCount[i] < quest->RequiredItemCount[i])
            incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
    }

    // Prefer the objective's real spawn positions over the map pin.
    //
    // quest_poi_points has no Z, so a pin can only ever say "somewhere around here on the ground".
    // Where the objective is above or below that ground -- a tower platform, a cave -- the bot
    // arrives at a place the pin describes correctly in two dimensions and finds nothing, then
    // patrols it indefinitely. The spawn tables know exactly where the targets are, height
    // included, so when they can answer they answer first.
    for (int32 objective : incompleteObjectiveIdx)
        AddSpawnCandidates(quest, objective, poiInfo);

    if (!poiInfo.empty())
        return true;

    // Get POIs to go
    for (const QuestPOI& qPoi : *poiVector)
    {
        if (qPoi.MapId != bot->GetMapId())
            continue;

        bool inComplete = false;
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (qPoi.ObjectiveIndex == static_cast<int32>(objective))
            {
                inComplete = true;
                break;
            }
        }
        if (!inComplete)
            continue;
        if (qPoi.points.empty())
            continue;

        AddPoiCandidates(qPoi, poiInfo);
    }

    if (poiInfo.size() == 0)
    {
        // LOG_DEBUG("playerbots", "[New rpg] {}: No available poi can be found for quest {}", bot->GetName(), questId);
        return false;
    }

    return true;
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    const std::vector<WorldLocation>& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        // Enemy territory policy: always skip their capital, and by default only rarely accept
        // anywhere else of theirs. See ShouldAvoidArea.
        if (ShouldAvoidArea(bot, sMapMgr->GetAreaId(bot->GetPhaseMask(), loc)))
            continue;

        if (bot->GetExactDist(loc) > sPlayerbotAIConfig.questObjectiveMaxDistance)
            continue;

        // P7.3 -- the same-zone restriction is gone. Quest objectives routinely sit just over a zone
        // border from their giver, and refusing those left bots holding quests they could physically
        // walk to in a minute but were forbidden to approach. The exception for bots standing in a
        // city was already in place, which suggests the restriction was known to be too strict.
        //
        // The *map* filter above stays: a point on another continent cannot be walked to, and until
        // objectives are routed through flight paths, selecting one would be a guaranteed stall.
        //
        // Distance is now the actual constraint, which is the honest one -- a bot is limited by how
        // far it can travel, not by lines drawn on a map. Configurable, because it trades quest
        // reach against time spent walking.
        (void)inCity;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    WorldPosition dest{};
    if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
        dest = *RandomElement(hi_prepared_locs);
    else if (WorldLocation const* loc = RandomElement(lo_prepared_locs))
        dest = *loc;

    // These locations come out of locsPerLevelCache, which stores grid cells scaled by 50 -- so the
    // Z is quantised to 50-yard steps and can sit tens of yards above or below the actual ground.
    //
    // That is fatal downstream rather than merely imprecise: GO_GRIND ends when
    // GetExactDist(pos) < 10, and GetExactDist is three-dimensional. A bot standing exactly on the
    // right spot still measures the full vertical error, never "arrives", and walks the same patch
    // of ground until the status times out -- with MoveFarTo teleporting it up to the phantom
    // height on the way, from which it simply falls back down.
    //
    // Quantised X and Y are fine, since the point is only ever "this region"; Z is the one axis a
    // walking bot cannot choose for itself, so it has to be resolved against the map.
    if (dest != WorldPosition())
        ResolveTeleportGround(bot, dest);

    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({}+{} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size() - hi_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    std::vector<WorldLocation> prepared_locs;
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (ShouldAvoidArea(bot, sMapMgr->GetAreaId(bot->GetPhaseMask(), loc)))
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        prepared_locs.push_back(loc);
    }
    WorldPosition dest{};
    if (WorldLocation const* loc = RandomElement(prepared_locs))
        dest = *loc;
    // Deliberately NOT ground-resolved. Unlike grind cells these are real NPC spawn coordinates and
    // are already correct -- and an inn keeper standing on an upper floor would be snapped down to
    // the terrain under the building, sending bots beneath it.
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = *RandomElement(availablePaths);
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

bool NewRpgBaseAction::HasTrainingBusiness()
{
    // A profession sitting at its rank cap is the signal that the next rank is worth buying. Below
    // the cap the bot can still improve by gathering, so a trip would accomplish nothing.
    //
    // Local list rather than PlayerbotFactory::tradeSkills, which is declared `uint32 tradeSkills[]`
    // with no size and therefore has no usable element count.
    static constexpr uint16 professionSkills[] = {
        SKILL_ALCHEMY,     SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
        SKILL_HERBALISM,   SKILL_INSCRIPTION,   SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING,
        SKILL_MINING,      SKILL_SKINNING,      SKILL_TAILORING,  SKILL_COOKING,
        SKILL_FIRST_AID,   SKILL_FISHING};

    for (uint16 skillId : professionSkills)
    {
        uint16 value = bot->GetSkillValue(skillId);
        if (!value)
            continue;

        if (value >= bot->GetMaxSkillValue(skillId))
            return true;
    }

    return false;
}

WorldPosition NewRpgBaseAction::SelectNearestTrainerPos(ObjectGuid& trainerGuid)
{
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    WorldPosition best{};
    float bestDist = FLT_MAX;

    for (ObjectGuid const& guid : npcs)
    {
        Unit* unit = ObjectAccessor::GetUnit(*bot, guid);
        if (!unit || !unit->HasNpcFlag(UNIT_NPC_FLAG_TRAINER))
            continue;

        if (unit->GetReactionTo(bot) <= REP_UNFRIENDLY)
            continue;

        float d = bot->GetExactDist(unit);
        if (d < bestDist)
        {
            bestDist = d;
            trainerGuid = guid;
            best = WorldPosition(unit->GetMapId(), unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
        }
    }

    return best;
}

bool NewRpgBaseAction::HasVendorBusiness()
{
    // Repair first: durability loss is the most common reason to need a vendor at all.
    if (bot->GetUInt32Value(PLAYER_FIELD_COINAGE) && bot->GetAverageItemLevel() > 0)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            uint32 maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            uint32 dur = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            if (maxDur && dur * 100 / maxDur < 60)
                return true;
        }
    }

    // Anything the shared classifier calls vendor fodder that AutoVendorJunk will not take.
    for (uint8 bag = INVENTORY_SLOT_ITEM_START; bag < INVENTORY_SLOT_ITEM_END; ++bag)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (!item)
            continue;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || !proto->SellPrice)
            continue;

        if (proto->Quality <= sPlayerbotAIConfig.autoVendorJunkMaxQuality)
            continue;  // AutoVendorJunk handles this without travelling

        if (AI_VALUE2(ItemUsage, "item usage", proto->ItemId) == ITEM_USAGE_VENDOR)
            return true;
    }

    return false;
}

WorldPosition NewRpgBaseAction::SelectNearestVendorPos()
{
    // Non-static and using AI_VALUE (which resolves through `context`) rather than PAI_VALUE, whose
    // macro hardcodes a variable literally named `player`.
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    WorldPosition best{};
    float bestDist = FLT_MAX;

    for (ObjectGuid const& guid : npcs)
    {
        Unit* unit = ObjectAccessor::GetUnit(*bot, guid);
        if (!unit || !unit->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
            continue;

        if (unit->GetReactionTo(bot) <= REP_UNFRIENDLY)
            continue;

        float d = bot->GetExactDist(unit);
        if (d < bestDist)
        {
            bestDist = d;
            best = WorldPosition(unit->GetMapId(), unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
        }
    }

    return best;
}

/**
 * A completed quest whose hand-in sits close by, or 0.
 *
 * "Close" is measured to the reward POI rather than to the giver's spawn, because that is the point
 * the client draws its question mark at and so the thing the operator is looking at when they say
 * the bot walked past one.
 */
/**
 * Which quest to work on first, lowest tier first.
 *
 * P7.4 made the *keeping* score asymmetric so a green quest is not shed as readily as a red one, but
 * the quest actually worked on was still drawn with RandomElement -- so a bot holding a green and a
 * red picked between them by coin flip.
 *
 * Green is the perishable one: it is the only tier that loses its experience outright, by turning
 * grey while the bot does something else. Yellow pays full value and is in no danger. Grey has
 * already lost what it had, but P7.5 keeps such quests because they are usually one step from done,
 * and finishing one frees a log slot cheaply -- so it goes ahead of the expensive tiers rather than
 * last. Orange and red cost the most attempts and stay viable longest, so they wait.
 *
 * Thresholds follow the client's own GetQuestDifficultyColor, so a bot's idea of "green" is the
 * colour the operator sees in their own quest log.
 */
int32 NewRpgBaseAction::QuestWorkPriority(uint32 questId, Quest const* quest)
{
    if (!quest)
        return 100;

    // Done is done: one interaction from experience and a reward, wherever it is.
    if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
        return -1;

    int32 const questLevel = static_cast<int32>(bot->GetQuestLevel(quest));
    int32 const botLevel = static_cast<int32>(bot->GetLevel());
    int32 const gap = questLevel - botLevel;

    uint32 const greyDiff = sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);
    bool const grey = botLevel > questLevel + static_cast<int32>(greyDiff);

    if (grey)
        return 2;
    if (gap >= 5)
        return 4;   // red
    if (gap >= 3)
        return 3;   // orange
    if (gap >= -2)
        return 1;   // yellow
    return 0;       // green -- the only tier with an expiry date
}

/**
 * P13.4 -- somewhere worth walking to inside an instance.
 *
 * A bot that zones into a dungeon keeps running its open-world brain: the activity roll picks quests,
 * gathering or the mailbox, none of which exist down here, so it cannot path anywhere and stands at
 * the door. Sixteen bots across seven instances were stacked on their entrance coordinates to the
 * decimal.
 *
 * The nearest living hostile is the honest target: it is the next pull, and walking to it is what
 * clearing a dungeon consists of. Returning a position rather than a unit lets the existing GO_GRIND
 * machinery do the travelling and hand over to WANDER_RANDOM to fight, so this adds a destination
 * rather than a second movement system.
 */
bool NewRpgBaseAction::IsEnemyArea(Player* bot, uint32 areaOrZoneId)
{
    if (!bot || !areaOrZoneId)
        return false;

    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaOrZoneId);
    if (!area)
        return false;

    // Only about seventy of the two thousand three hundred areas name a side at all, and they are
    // almost exactly the faction levelling zones -- Elwynn and Westfall to the Alliance, Tirisfal
    // and Silverpine and the blood elf zones to the Horde. Everywhere else is nobody's, which is
    // why this is a narrow rule rather than a map-wide one.
    uint32 const owner = area->team;
    if (owner == AREATEAM_NONE || owner == AREATEAM_ANY)
        return false;

    bool const botIsAlliance = bot->GetTeamId() == TEAM_ALLIANCE;
    bool const areaIsAlliance = owner == AREATEAM_ALLY;
    return botIsAlliance != areaIsAlliance;
}

bool NewRpgBaseAction::IsEnemyCapital(Player* bot, uint32 areaOrZoneId)
{
    if (!IsEnemyArea(bot, areaOrZoneId))
        return false;

    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaOrZoneId);
    return area && (area->flags & AREA_FLAG_CAPITAL) != 0;
}

bool NewRpgBaseAction::ShouldAvoidArea(Player* bot, uint32 areaOrZoneId)
{
    if (!IsEnemyArea(bot, areaOrZoneId))
        return false;

    // A capital is not a destination, it is a death: wall-to-wall high level guards that aggro on
    // sight and nothing in it for a visitor. Nobody plays that way on purpose, so nothing routes
    // there on purpose, whatever the policy for ordinary territory is.
    if (IsEnemyCapital(bot, areaOrZoneId))
        return true;

    switch (sPlayerbotAIConfig.enemyTerritoryPolicy)
    {
        case 0:   // allow
            return false;
        case 2:   // block
            return true;
        default:  // discourage
            break;
    }

    // Rolled per candidate rather than filtered outright, so enemy ground stays reachable but rare.
    // A bot that occasionally turns up somewhere it has no business being is the kind of thing that
    // makes a realm look inhabited; one that does it constantly is the bug this came from.
    return urand(1, 100) > sPlayerbotAIConfig.enemyTerritoryChance;
}

/**
 * Talk to the NPC standing between the group and the rest of the dungeon.
 *
 * Not every door is opened by killing something. Shadowfang Keep's courtyard door is the clearest
 * case: Rethilgore dies, the group has nothing left to pull, and the door stays shut because the
 * actual mechanism is a gossip option on the prisoner in the cells -- SMART_EVENT_GOSSIP_SELECT on
 * Deathstalker Adamant or Sorcerer Ashcrombe, who then walks to the door and unlocks it. A group of
 * bots cleared the room and stood there for as long as anyone let them.
 *
 * Called only when the pull search has already come up empty, which is the honest signal that
 * progress needs something other than violence. Restricted to instances, to friendly creatures that
 * actually offer a gossip menu, and to NPCs whose flags say talking is all they are for -- a vendor
 * or a trainer offers gossip too, and buying something is not progress.
 *
 * @return true if the bot moved toward an NPC or talked to one.
 */
bool NewRpgBaseAction::TryDungeonGossip()
{
    Map* map = bot->FindMap();
    if (!map || !map->Instanceable())
        return false;

    std::list<Creature*> nearby;
    Acore::AnyUnitInObjectRangeCheck check(bot, sPlayerbotAIConfig.dungeonPullSearchRange);
    Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, nearby, check);
    Cell::VisitObjects(bot, searcher, sPlayerbotAIConfig.dungeonPullSearchRange);

    // Anything that sells, trains, repairs, flies or banks is not a door mechanism. Innkeepers and
    // auctioneers likewise. What is left in an instance is the NPC that is there to be spoken to.
    constexpr uint32 NOT_PROGRESSION = UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_REPAIR |
                                       UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_BANKER |
                                       UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_AUCTIONEER |
                                       UNIT_NPC_FLAG_SPIRITHEALER | UNIT_NPC_FLAG_STABLEMASTER;

    Creature* talkTo = nullptr;
    float bestDist = FLT_MAX;

    for (Creature* creature : nearby)
    {
        if (!creature || !creature->IsAlive() || creature->IsHostileTo(bot))
            continue;

        if (!creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) || creature->HasNpcFlag(NPCFlags(NOT_PROGRESSION)))
            continue;

        float const dist = bot->GetDistance(creature);
        if (dist < bestDist)
        {
            bestDist = dist;
            talkTo = creature;
        }
    }

    if (!talkTo)
        return false;

    if (bestDist > INTERACTION_DISTANCE)
    {
        LOG_DEBUG("playerbots", "[Dungeon] {} nothing to pull; walking to {} to talk", bot->GetName(),
                  talkTo->GetName());
        return MoveTo(map->GetId(), talkTo->GetPositionX(), talkTo->GetPositionY(), talkTo->GetPositionZ(),
                      false, false, false, true);
    }

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    bot->SetFacingToObject(talkTo);

    WorldPacket hello(CMSG_GOSSIP_HELLO);
    hello << talkTo->GetGUID();
    bot->GetSession()->HandleGossipHelloOpcode(hello);

    GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
    if (!menu.GetMenuItemCount())
        return false;

    // The first option. These unlock NPCs offer exactly one thing, and guessing among several is
    // not something to do blind -- if a menu ever has more, taking the first is still the option a
    // person clicking through would take.
    WorldPacket select(CMSG_GOSSIP_SELECT_OPTION);
    select << talkTo->GetGUID() << uint32(menu.GetMenuId()) << uint32(0);
    bot->GetSession()->HandleGossipSelectOptionOpcode(select);

    LOG_DEBUG("playerbots", "[Dungeon] {} talked to {} (menu {}) with nothing left to pull", bot->GetName(),
              talkTo->GetName(), menu.GetMenuId());
    return true;
}

WorldPosition NewRpgBaseAction::SelectDungeonPullPos()
{
    Map* map = bot->FindMap();
    if (!map || !map->Instanceable())
        return WorldPosition();

    std::list<Unit*> targets;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, sPlayerbotAIConfig.dungeonPullSearchRange);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, targets, check);
    Cell::VisitObjects(bot, searcher, sPlayerbotAIConfig.dungeonPullSearchRange);

    // Candidates that pass the cheap tests, nearest first. The expensive test -- asking mmap for a
    // route -- then runs over as few of them as possible.
    std::vector<std::pair<float, Unit*>> candidates;

    uint32 rejectedEngaged = 0;
    uint32 rejectedKind = 0;
    uint32 rejectedTarget = 0;

    for (Unit* unit : targets)
    {
        if (!unit || !unit->IsAlive() || unit->IsPlayer())
            continue;

        // Already engaged is not a pull; the combat engine is handling it.
        if (unit->IsInCombat())
        {
            ++rejectedEngaged;
            continue;
        }

        Creature* creature = unit->ToCreature();
        if (!creature || creature->IsCritter() || creature->IsTotem())
        {
            ++rejectedKind;
            continue;
        }

        // Walk to something the bot will actually be willing to fight when it arrives. This search
        // and the one behind "attack anything" used to answer different questions -- one asked "is
        // there a creature over there", the other "may I engage this" -- so a leader walked to a
        // pack it was never going to pull, arrived, found nothing, and wandered on the spot.
        if (!AttackersValue::IsPossibleTarget(unit, bot, sPlayerbotAIConfig.dungeonPullSearchRange) ||
            !bot->IsHostileTo(unit))
        {
            ++rejectedTarget;
            continue;
        }

        candidates.emplace_back(bot->GetDistance(unit), unit);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    // Reachability, not line of sight.
    //
    // This check used to be IsWithinLOSInMap. Line of sight answers "can I attack this from where I
    // stand", which is the right question for the combat engine and the wrong one for deciding
    // where a group goes next: clearing a dungeon means walking to packs you cannot currently see,
    // because they are behind the door you have not opened yet. Ragefire Chasm is one open spiral,
    // so it happened to work there; Shadowfang Keep is rooms and doorways, and a group stood at the
    // entrance reporting "resting" with the first pack two rooms away and plainly visible on the
    // map, just not from that spot.
    //
    // Asking mmap whether the leader can walk there keeps the property line of sight was added for
    // -- a pack through a wall or on the floor above has no route, so the leader still will not set
    // off toward one -- while letting the group round a corner.
    //
    // Bounded twice over, because this is the expensive part of the search: only the nearest few
    // candidates are tested, and a route far longer than the straight line is treated as no route,
    // which rejects the case where mmap loops the entire level to reach something on the other side
    // of a wall.
    // Both of these were first set by guesswork and then corrected by the diagnostic below, which
    // reported a Wailing Caverns leader looking at thirty perfectly good candidates, testing six,
    // and resting. A cap that stops before it reaches a reachable target is indistinguishable from
    // an empty room.
    //
    // Sixteen is enough to get past a wall of near-but-unreachable mobs -- the far side of a chasm,
    // the floor below -- without the search becoming unbounded. It only ever runs for the one bot
    // leading a group inside an instance.
    constexpr size_t MAX_PATH_TESTS = 16;

    // A winding tunnel is a real route, not a detour to reject. Wailing Caverns and Maraudon both
    // routinely path three or four times the straight-line distance, and for a target a few yards
    // away through a doorway the ratio is meaningless -- hence the floor, which matters more than
    // the factor at close range.
    constexpr float MAX_DETOUR_FACTOR = 4.0f;
    constexpr float MIN_DETOUR_ALLOWANCE = 80.0f;

    uint32 rejectedUnreachable = 0;
    size_t tested = 0;

    for (auto const& [dist, unit] : candidates)
    {
        if (tested++ >= MAX_PATH_TESTS)
            break;

        PathGenerator path(bot);
        path.CalculatePath(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());

        PathType const type = path.GetPathType();
        if (!(type & PATHFIND_NORMAL) || (type & PATHFIND_NOPATH))
        {
            ++rejectedUnreachable;
            continue;
        }

        if (path.getPathLength() > std::max(dist * MAX_DETOUR_FACTOR, MIN_DETOUR_ALLOWANCE))
        {
            ++rejectedUnreachable;
            continue;
        }

        return WorldPosition(map->GetId(), unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
    }

    // Says which test emptied the list. "Found nothing" covered four very different situations --
    // an empty room, a pack already in combat, a pack that cannot be attacked, and one with no
    // route -- and they need different fixes.
    if (!candidates.empty() || rejectedEngaged || rejectedKind || rejectedTarget)
        LOG_DEBUG("playerbots",
                  "[Dungeon] {} pull search: {} candidate(s), {} routed, rejected {} engaged, {} kind, "
                  "{} untargetable, {} unreachable",
                  bot->GetName(), uint32(candidates.size()), uint32(tested), rejectedEngaged, rejectedKind,
                  rejectedTarget, rejectedUnreachable);

    return WorldPosition();
}

bool NewRpgBaseAction::DungeonGroupReady()
{
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    // Only the people actually in the run. A member left in town, or a zone away, is not someone
    // the leader can wait for.
    constexpr float NEARBY = 100.0f;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() ||
            member->GetInstanceId() != bot->GetInstanceId() || bot->GetExactDist(member) > NEARBY)
            continue;

        char const* reason = nullptr;
        if (!member->IsAlive())
            reason = "is dead";
        else if (member->IsInCombat())
            reason = "is still fighting";
        else if (member->GetHealthPct() < sPlayerbotAIConfig.dungeonReadyHealthPct)
            reason = "is low on health";
        else if (member->getPowerType() == POWER_MANA && PlayerbotAI::IsHeal(member) &&
                 member->GetPowerPct(POWER_MANA) < sPlayerbotAIConfig.dungeonReadyHealerManaPct)
            reason = "is drinking";

        if (reason)
        {
            // Throttled to one line a minute per leader: this is asked every status update while
            // the group rests, and the reason rarely changes between asks.
            if (getMSTimeDiff(_lastReadyLogMs, getMSTime()) > 60 * IN_MILLISECONDS || !_lastReadyLogMs)
            {
                _lastReadyLogMs = getMSTime();
                LOG_DEBUG("playerbots", "[Dungeon] {} waits before the next pull: {} {} ({:.0f}% health, {:.0f}% mana)",
                          bot->GetName(), member->GetName(), reason, member->GetHealthPct(),
                          member->getPowerType() == POWER_MANA ? member->GetPowerPct(POWER_MANA) : 100.0f);
            }
            return false;
        }
    }

    return true;
}

WorldPosition NewRpgBaseAction::SelectDungeonAdvancePos()
{
    Map* map = bot->FindMap();
    if (!map || !map->Instanceable())
        return WorldPosition();

    // The whole instance, not the 120 yards around the leader.
    //
    // The pull search looks only as far as the leader can reasonably see, which is right for choosing
    // the next pack and wrong for deciding the run is over. After most kills in Wailing Caverns the
    // next pack is further along a tunnel than that, so the search came back empty and the leader sat
    // down to rest for thirty seconds -- after almost every kill, with the next pack a short walk away.
    // Toggling self bot mode off and on made her move again only because it reset the rest.
    //
    // A dungeon holds a few hundred creatures at most and this only runs for a group's leader after
    // the local search has already come up empty, so walking the instance's own creature store is
    // cheap enough; only the nearest few are asked for a route.
    constexpr size_t MAX_ADVANCE_PATH_TESTS = 12;
    constexpr float ADVANCE_SEARCH_RANGE = 2000.0f;

    std::vector<std::pair<float, Creature*>> candidates;
    for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
    {
        if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->IsInCombat())
            continue;

        if (creature->IsCritter() || creature->IsTotem() || !bot->IsHostileTo(creature))
            continue;

        if (botAI->IsUnreachableTarget(creature->GetGUID()))
            continue;

        if (!AttackersValue::IsPossibleTarget(creature, bot, ADVANCE_SEARCH_RANGE))
            continue;

        candidates.emplace_back(bot->GetDistance(creature), creature);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    size_t tested = 0;
    for (auto const& [dist, creature] : candidates)
    {
        if (tested++ >= MAX_ADVANCE_PATH_TESTS)
            break;

        PathGenerator path(bot);
        path.CalculatePath(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());

        // No detour cap here, unlike the pull search: this is the route to the next part of the
        // dungeon, and in a cave the next part of the dungeon is very often the long way round.
        PathType const type = path.GetPathType();
        if (!(type & PATHFIND_NORMAL) || (type & PATHFIND_NOPATH))
            continue;

        return WorldPosition(map->GetId(), creature->GetPositionX(), creature->GetPositionY(),
                             creature->GetPositionZ());
    }

    return WorldPosition();
}

/**
 * Is this activity allowed for this bot at all?
 *
 * The same test RandomChangeStatus applies before the weighted roll: a base weight of zero means the
 * operator has switched the activity off, and an archetype may re-enable it for the disposition it
 * belongs to. Pulled out so the state machine's own transitions can respect it too -- they were
 * bypassing it entirely.
 */
bool NewRpgBaseAction::IsRpgStatusPermitted(NewRpgStatus status)
{
    int32 const activityOverride = sBotAgendaMgr.GetActivityBaseOverride(bot, status);
    uint32 const base = activityOverride > 0 ? uint32(activityOverride)
                                             : sPlayerbotAIConfig.RpgStatusProbWeight[status];
    return base > 0;
}

uint32 NewRpgBaseAction::FindNearbyTurnIn()
{
    // Once committed, stay committed.
    //
    // Distance alone is not a stable test. The bot commits inside the radius, DO_QUEST times out
    // part way through the walk, and by the next idle it is *outside* the radius -- so the
    // preemption goes quiet, it picks some other errand, drifts back inside, and commits again.
    // The operator saw exactly that: a bot running at a quest giver, away to the Darkmoon tents,
    // and back, for a long while.
    //
    // Holding the choice until the quest is actually handed in is what a person does, and it costs
    // nothing: the quest leaving the log clears it, so this cannot latch onto something stale.
    if (_committedTurnIn)
    {
        if (bot->GetQuestStatus(_committedTurnIn) == QUEST_STATUS_COMPLETE)
        {
            std::vector<POIInfo> held;
            if (GetQuestPOIPosAndObjectiveIdx(_committedTurnIn, held, true))
                return _committedTurnIn;
        }

        _committedTurnIn = 0;
    }

    uint32 nearest = 0;
    float nearestDist = sPlayerbotAIConfig.questTurnInPriorityDistance;

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;

        if (sQuestBlacklistMgr.IsBlacklisted(questId))
            continue;

        // toComplete: this asks for the hand-in location specifically, not the objectives.
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
            continue;

        for (POIInfo const& poi : poiInfo)
        {
            float const dist = bot->GetDistance2d(poi.pos.x, poi.pos.y);
            if (dist < nearestDist)
            {
                nearestDist = dist;
                nearest = questId;
            }
        }
    }

    _committedTurnIn = nearest;
    return nearest;
}

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus)
{
    // Someone is stuck and this bot can reach them. Answering outranks the weighted roll entirely:
    // a call for help is the one thing on the realm that is genuinely time-sensitive, and a bot that
    // wanders off to vendor instead is the behaviour that makes the world look uninhabited.
    //
    // Answering is expressed as a grind at the caller's position rather than a new activity type.
    // That is not a shortcut: travelling there co-locates the two bots, and the proximity invite
    // from P7.8 -- which already works and only ever lacked candidates -- then forms the group
    // without any further mechanism.
    if (BotHelpMgr::Request request; sBotHelpMgr.FindRequestFor(bot, request))
    {
        WorldPosition helpPos(request.mapId, request.x, request.y, request.z);
        if (helpPos != WorldPosition())
        {
            sBotHelpMgr.AcceptRequest(request.caller, bot->GetGUID());
            LOG_DEBUG("playerbots", "[Help] {} (level {}) answering {} for quest {}", bot->GetName(),
                      bot->GetLevel(), request.caller.GetCounter(), request.questId);

            botAI->rpgInfo.ChangeToGoGrind(helpPos);
            return true;
        }
    }

    // P13.4 -- inside an instance the open-world activity mix is meaningless, and rolling it is how
    // a group ends up standing on the entrance while its brain tries to go gather herbs. Only the
    // leader navigates: everyone else is already following it, and five bots each choosing their own
    // pull is not a group clearing a dungeon.
    if (Map* map = bot->FindMap(); map && map->Instanceable())
    {
        Group* group = bot->GetGroup();
        bool const leads = !group || group->GetLeaderGUID() == bot->GetGUID();

        if (leads && !DungeonGroupReady())
        {
            // Resting is what lets the healer drink; pulling now leaves them behind.
            botAI->rpgInfo.ChangeToRest();
            return true;
        }

        if (leads)
        {
            WorldPosition pull = SelectDungeonPullPos();
            if (pull != WorldPosition())
            {
                // Logged because there was no way to tell this apart from a group simply standing
                // still: the whole branch was silent, so a run that never entered it and a run where
                // it fired constantly produced identical logs.
                LOG_DEBUG("playerbots", "[Dungeon] {} leads a pull on map {} to ({:.0f},{:.0f},{:.0f}), {:.0f} yards off",
                          bot->GetName(), map->GetId(), pull.GetPositionX(), pull.GetPositionY(),
                          pull.GetPositionZ(), bot->GetExactDist(pull));

                botAI->rpgInfo.ChangeToGoGrind(pull);
                return true;
            }

            // Nothing close enough to pull is not the same as nothing left. Walk on to the nearest pack
            // the instance still holds, rather than sitting down for thirty seconds after every kill.
            WorldPosition advance = SelectDungeonAdvancePos();
            if (advance != WorldPosition())
            {
                LOG_DEBUG("playerbots", "[Dungeon] {} advances on map {} towards the next pack, {:.0f} yards off",
                          bot->GetName(), map->GetId(), bot->GetExactDist(advance));

                botAI->rpgInfo.ChangeToGoGrind(advance);
                return true;
            }

            LOG_DEBUG("playerbots", "[Dungeon] {} finds nothing left to fight on map {} -- the run looks clear",
                      bot->GetName(), map->GetId());

            // Nothing to fight is not the same as nothing to do. Try the NPC before giving up.
            if (TryDungeonGossip())
                return true;
        }

        // Nothing left to pull, or not the leader. Resting beats wandering off in a dungeon.
        botAI->rpgInfo.ChangeToRest();
        return true;
    }

    // A finished quest with its turn-in within reach outranks the weighted roll, the same way a call
    // for help does. This is the question mark on the minimap: the bot is standing next to the one
    // action that converts everything it has already done into a level, and rolling dice against
    // vendoring and grinding instead is how an operator watches a bot carry two completed quests
    // past their giver.
    //
    // Deliberately near-only. Anything further is ordinary travel and belongs in the weighted roll
    // with everything else; overriding at any distance would turn every completed quest into an
    // errand that pre-empts the whole activity mix.
    if (uint32 const turnIn = FindNearbyTurnIn())
    {
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(turnIn))
        {
            LOG_DEBUG("playerbots", "[New RPG] {} has quest {} complete and its turn-in nearby -- going now",
                      bot->GetName(), turnIn);
            botAI->rpgInfo.ChangeToDoQuest(turnIn, quest);
            return true;
        }
    }

    // Base weight from config, scaled by what this bot is actually trying to achieve. Without the
    // agenda multiplier every bot on the realm draws from one global table, so a gatherer saving for
    // a mount and a fresh level-5 quester pick their next activity from identical odds.
    std::vector<NewRpgStatus> availableStatus;
    std::vector<uint32> weights;
    uint32 probSum = 0;

    for (NewRpgStatus status : candidateStatus)
    {
        // An archetype may insist on an activity the global table has switched off. That is the
        // only way to express "nobody does this except the disposition it belongs to", since a zero
        // weight is dropped here, before any multiplier could revive it.
        int32 const override = sBotAgendaMgr.GetActivityBaseOverride(bot, status);
        uint32 const base = override > 0 ? uint32(override) : sPlayerbotAIConfig.RpgStatusProbWeight[status];

        if (base == 0)
            continue;

        if (!CheckRpgStatusAvailable(status))
            continue;

        // Rounded up, so a goal that merely discourages an activity cannot silently zero it out and
        // make the activity unreachable.
        uint32 const weight =
            std::max<uint32>(1, static_cast<uint32>(base * sBotAgendaMgr.GetActivityMultiplier(bot, status)));

        availableStatus.push_back(status);
        weights.push_back(weight);
        probSum += weight;
    }
    // Safety check. Default to "rest" if all RPG weights = 0
    if (availableStatus.empty() || probSum == 0)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    uint32 rand = urand(1, probSum);
    uint32 accumulate = 0;
    NewRpgStatus chosenStatus = RPG_STATUS_END;
    for (size_t i = 0; i < availableStatus.size(); ++i)
    {
        accumulate += weights[i];
        if (accumulate >= rand)
        {
            chosenStatus = availableStatus[i];
            break;
        }
    }

    // Tagged with the archetype so the realm's activity mix can be broken down per disposition.
    // P6.3's acceptance is "a Gatherer and a Socialite visibly differ", which is a claim about
    // distributions and cannot be settled by watching one bot.
    LOG_DEBUG("playerbots", "[Activity] {} [{}] chose {}", bot->GetName(), sBotAgendaMgr.GetArchetypeLabel(bot),
              static_cast<int>(chosenStatus));

    switch (chosenStatus)
    {
        case RPG_WANDER_RANDOM:
        {
            botAI->rpgInfo.ChangeToWanderRandom();
            return true;
        }
        case RPG_WANDER_NPC:
        {
            botAI->rpgInfo.ChangeToWanderNpc();
            return true;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoGrind(pos);
                return true;
            }
            return false;
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoCamp(pos);
                return true;
            }
            return false;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (sQuestBlacklistMgr.IsBlacklisted(questId))
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    availableQuests.push_back(questId);
                }
            }
            if (availableQuests.size())
            {
                // Best tier first, then at random within it. Keeping the draw inside the tier
                // matters: a whole realm of bots working their quest logs in a fixed order would
                // converge on the same objectives, and the randomness was doing that job even while
                // it was ignoring difficulty.
                int32 bestTier = 1000;
                for (uint32 candidate : availableQuests)
                    bestTier = std::min(bestTier,
                                        QuestWorkPriority(candidate, sObjectMgr->GetQuestTemplate(candidate)));

                std::vector<uint32> preferred;
                for (uint32 candidate : availableQuests)
                    if (QuestWorkPriority(candidate, sObjectMgr->GetQuestTemplate(candidate)) == bestTier)
                        preferred.push_back(candidate);

                uint32 questId = *RandomElement(preferred);
                const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest)
                {
                    botAI->rpgInfo.ChangeToDoQuest(questId, quest);
                    return true;
                }
            }
            return false;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
            {
                botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
                return true;
            }
            return false;
        }
        case RPG_IDLE:
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        case RPG_REST:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            botAI->rpgInfo.ChangeToOutdoorPvp();
            return true;
        }
        case RPG_VENDOR:
        {
            WorldPosition pos = SelectNearestVendorPos();
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToVendor(pos);
                return true;
            }
            return false;
        }
        case RPG_MAILBOX:
        {
            botAI->rpgInfo.ChangeToMailbox();
            return true;
        }
        case RPG_TRAIN:
        {
            ObjectGuid trainerGuid;
            WorldPosition pos = SelectNearestTrainerPos(trainerGuid);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToTrain(pos, trainerGuid);
                return true;
            }
            return false;
        }
        case RPG_GATHER:
        {
            if (GatherRouteMgr::Route const* route = sGatherRouteMgr.PickRouteWithinReach(bot))
            {
                // The route's zone, not the bot's. They differ whenever the bot is somewhere with
                // nothing to gather -- which is most of why this activity gets chosen at all -- and
                // storing the bot's zone would send the activity looking for a route in the city it
                // is trying to leave.
                float const travel = route->nodes.empty()
                                         ? 0.0f
                                         : bot->GetDistance2d(route->nodes.front().x, route->nodes.front().y);
                // Climb as well as distance. A route 380 yards away is unremarkable; the same route
                // 380 yards away and 150 yards up is a mountaineering trip, and the two are
                // indistinguishable in a log that only reports the horizontal.
                if (ShouldAvoidArea(bot, route->zoneId))
                {
                    LOG_DEBUG("playerbots", "[GatherStart] {} skipped route in zone {}: enemy territory",
                              bot->GetName(), route->zoneId);
                    return false;
                }

                float const climb = route->nodes.empty()
                                        ? 0.0f
                                        : route->nodes.front().z - bot->GetPositionZ();
                LOG_DEBUG("playerbots",
                          "[GatherStart] {} zone {} -> route zone {}, {:.0f} yards to first node "
                          "({:+.0f} yards of climb)",
                          bot->GetName(), bot->GetZoneId(), route->zoneId, travel, climb);

                botAI->rpgInfo.ChangeToGather(route->zoneId, route->skillId);
                return true;
            }
            return false;
        }
        case RPG_CRAFT_GOAL:
        {
            // The roll picked this, so now the goal is stored -- not when availability merely offered it.
            std::optional<CraftGoalMgr::Goal> const goal = sCraftGoalMgr.Choose(bot, true);
            if (!goal)
                return false;

            CraftGoalMgr::Need const need = sCraftGoalMgr.NextNeed(bot, *goal);
            uint32 zoneId = bot->GetZoneId();
            uint32 sourceEntry = 0;
            WorldPosition pos;
            if (need.itemId)
                if (ReagentSourceMgr::Source const* source = sReagentSourceMgr.BestFor(bot, need.itemId))
                {
                    zoneId = source->zoneId;
                    sourceEntry = source->sourceEntry;
                    pos = WorldPosition(source->mapId, source->x, source->y, source->z);
                }

            botAI->rpgInfo.ChangeToCraftGoal(need.itemId, zoneId, sourceEntry, pos);
            return true;
        }
        case RPG_FISH:
        {
            // No position to pick here. Availability already established that water is within
            // casting reach, and the activity's own first tick asks MoveNearWaterAction for the
            // exact shoreline to stand on -- which is the same search, so doing it twice would only
            // give two answers to disagree about.
            botAI->rpgInfo.ChangeToFish();
            return true;
        }
        default:
        {
            // Reached legitimately when the weighted roll found nothing available, and chosenStatus
            // is RPG_STATUS_END. Reached by mistake when a status was chosen that this switch has no
            // case for -- which is silent, because resting is a plausible thing for a bot to do.
            //
            // RPG_FISH was added to the enum, the weight table, the availability check, the action,
            // the trigger and the strategy, and missed only here; every bot that rolled fishing sat
            // down instead, and six status samples of `Fish: 0` were the only symptom.
            if (chosenStatus != RPG_STATUS_END)
                LOG_WARN("playerbots", "[New RPG] {} chose status {} which has no transition -- resting instead",
                         bot->GetName(), static_cast<int>(chosenStatus));

            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::TryDeliberateDrop(WorldPosition const& dest)
{
    if (!sPlayerbotAIConfig.deliberateDropEnabled)
        return false;

    // Mirrors Player.cpp's fall equation. Those constants are file-local `static constexpr` there,
    // so they cannot be included -- and a copy is a drift risk worth naming rather than hiding: if
    // the core's numbers ever change, this is the other place to change. Keeping them is still
    // better than inventing a threshold, because a guess that disagrees with the real damage either
    // strands bots on ledges or kills them stepping off one.
    constexpr float FALL_DMG_SLOPE = 0.018f;
    constexpr float FALL_DMG_INTERCEPT = -0.2426f;
    constexpr float MIN_FALL_DMG_DIST = 13.48f;

    // Below this there is nothing to solve: a drop the bot can simply walk down is one the navmesh
    // already has an edge for.
    constexpr float MIN_USEFUL_DROP = 5.0f;

    if (bot->IsFlying() || bot->IsMounted() || bot->isSwimming() || bot->IsInCombat())
        return false;

    Map* map = bot->GetMap();
    if (!map)
        return false;

    // Step off toward the destination rather than straight down: the edge is almost always in the
    // direction the bot has been failing to walk, and landing a few yards along the way is free.
    float const angle = bot->GetAngle(dest.GetPositionX(), dest.GetPositionY());
    float const stepX = bot->GetPositionX() + std::cos(angle) * sPlayerbotAIConfig.deliberateDropStep;
    float const stepY = bot->GetPositionY() + std::sin(angle) * sPlayerbotAIConfig.deliberateDropStep;

    float const groundZ = map->GetHeight(bot->GetPhaseMask(), stepX, stepY, bot->GetPositionZ(), true, 200.0f);
    if (groundZ <= INVALID_HEIGHT)
        return false;

    float const drop = bot->GetPositionZ() - groundZ;
    if (drop < MIN_USEFUL_DROP)
        return false;

    // Do not jump *down* into something further from the goal than where we stand.
    if (dest.GetPositionZ() > groundZ + MIN_USEFUL_DROP)
        return false;

    float survivingPct = 1.0f;
    if (drop >= MIN_FALL_DMG_DIST)
    {
        int32 const safeFall = bot->GetTotalAuraModifier(SPELL_AURA_SAFE_FALL);
        float const damagePct =
            std::max(0.0f, FALL_DMG_SLOPE * (drop - float(safeFall)) + FALL_DMG_INTERCEPT) *
            sWorld->getRate(RATE_DAMAGE_FALL);

        float const healthPct = bot->GetMaxHealth() ? float(bot->GetHealth()) / float(bot->GetMaxHealth()) : 0.0f;
        survivingPct = healthPct - damagePct;
    }

    if (survivingPct < sPlayerbotAIConfig.deliberateDropMinHealthPct)
    {
        LOG_DEBUG("playerbots", "[Drop] {} refused a {:.0f} yard drop: would land at {:.0f}% health",
                  bot->GetName(), drop, survivingPct * 100.0f);
        return false;
    }

    // MoveJump, not a teleport. The point of the requirement is that the bot uses the world rather
    // than stepping around it, and a jump is a real movement the client sees and the fall damage
    // actually applies to.
    bot->GetMotionMaster()->MoveJump(stepX, stepY, groundZ, sPlayerbotAIConfig.deliberateDropStep, 0.0f);

    LOG_INFO("playerbots", "[Drop] {} stepped off a {:.0f} yard ledge toward its destination, landing at ~{:.0f}% health",
             bot->GetName(), drop, survivingPct * 100.0f);
    return true;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        {
            Unit* target = AI_VALUE(Unit*, "grind target");
            return target != nullptr;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            return pos != WorldPosition();
        }
        case RPG_WANDER_NPC:
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            return possibleTargets.size() >= 3;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (sQuestBlacklistMgr.IsBlacklisted(questId))
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_VENDOR:
        {
            // Only worth a trip if there is something to offload or repair. AutoVendorJunk already
            // clears greys with no travel, so this is for whites and for durability.
            if (HasVendorBusiness())
                return SelectNearestVendorPos() != WorldPosition();
            return false;
        }
        case RPG_MAILBOX:
            return !bot->GetMails().empty();
        case RPG_TRAIN:
        {
            if (!HasTrainingBusiness())
                return false;
            ObjectGuid guid;
            return SelectNearestTrainerPos(guid) != WorldPosition();
        }
        case RPG_CRAFT_GOAL:
        {
            // Offered only when there is a goal whose next step can actually be worked. Choosing the
            // goal here rather than in the dispatch keeps the decision in one place: if nothing is
            // worth farming, the activity is simply never offered.
            std::optional<CraftGoalMgr::Goal> const goal = sCraftGoalMgr.Choose(bot, false);
            if (!goal)
                return false;

            CraftGoalMgr::Need const need = sCraftGoalMgr.NextNeed(bot, *goal);
            if (!need.itemId)
                return true;  // everything present; the activity crafts it on its first tick

            return need.farmable;
        }
        case RPG_FISH:
        {
            // Fish where you happen to be, rather than trekking to the nearest lake. That is both
            // what a person does and what keeps this affordable: the water search below is the
            // expensive part, so every cheap disqualifier runs ahead of it.
            if (!bot->GetSkillValue(SKILL_FISHING))
                return false;

            // A random bot is handed a pole on the spot; anyone else has to own one. The tool
            // manager buys it, so this only defers fishing until the next shopping pass.
            if (!sRandomPlayerbotMgr.IsRandomBot(bot) && !BotToolMgr::HasFishingPole(bot))
                return false;

            if (bot->IsInCombat() || bot->isSwimming() || bot->IsMounted())
                return false;

            if (bot->GetFreeInventorySpace() < sPlayerbotAIConfig.fishingMinFreeBagSlots)
                return false;

            // Measured exactly the way MoveNearWaterAction measures it, so an availability test
            // cannot offer a bot water it will then decide it cannot reach.
            WorldPosition water = FindWaterRadial(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                  bot->GetPositionZ(), bot->GetMap(), bot->GetPhaseMask(),
                                                  MIN_DISTANCE_TO_WATER,
                                                  sPlayerbotAIConfig.fishingDistance + MAX_DISTANCE_TO_WATER,
                                                  SEARCH_INCREMENT, false);
            if (!water.IsValid())
                return false;

            LOG_DEBUG("playerbots", "[Fish] {} available: water within reach in zone {}", bot->GetName(),
                      bot->GetZoneId());
            return true;
        }
        case RPG_GATHER:
        {
            // Needs a gathering profession, a route in this zone the bot's skill can work, and
            // somewhere to put what it picks up.
            //
            // Logged with the reason: an availability check that silently returns false is exactly
            // how gathering stayed broken for three test cycles while looking configured correctly.
            if (bot->GetFreeInventorySpace() < sPlayerbotAIConfig.gatheringMinFreeBagSlots)
            {
                LOG_DEBUG("playerbots", "[GatherAvail] {} denied: only {} free bag slots (needs {})",
                          bot->GetName(), bot->GetFreeInventorySpace(),
                          sPlayerbotAIConfig.gatheringMinFreeBagSlots);
                return false;
            }

            if (!sGatherRouteMgr.PickRouteWithinReach(bot))
            {
                LOG_DEBUG("playerbots", "[GatherAvail] {} denied: no usable route within reach of zone {}",
                          bot->GetName(), bot->GetZoneId());
                return false;
            }

            LOG_DEBUG("playerbots", "[GatherAvail] {} available in zone {}", bot->GetName(), bot->GetZoneId());
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        default:
            return false;
    }
    return false;
}
