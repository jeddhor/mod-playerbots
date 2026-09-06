/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DatabaseEnv.h"
#include "GameTime.h"
#include "NewRpgAction.h"
#include "GatherRouteMgr.h"
#include "Item.h"
#include "Mail.h"
#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "GossipDef.h"
#include "RandomUtils.h"
#include "IVMapMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "QuestBlacklistMgr.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "G3D/Vector2.h"
#include <cmath>
#include <cstdlib>

void TellRpgStatusAction::WhisperStatusChange(Player* owner, std::string const& statusName)
{
    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        RPG_STATUS_CHANGED_KEY, RPG_STATUS_CHANGED_DEFAULT,
        {{"%status", statusName}});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
}

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    if (text.empty())
    {
        std::string out = botAI->rpgInfo.ToString();
        bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
        return true;
    }

    Player* master = botAI->GetMaster();
    bool isMaster = master && master->GetGUID() == owner->GetGUID();
    bool isGM = owner->GetSession() && owner->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    if (!isMaster && !isGM)
    {
        std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "rpg_debug_permission_error",
            "Only your master or a GM can change my rpg status.", {});
        bot->Whisper(msg, LANG_UNIVERSAL, owner);
        return false;
    }

    std::string name = text;
    uint32 questId = 0;
    static std::string const doQuestPrefix = "do quest ";
    size_t doQuestPos = text.find(doQuestPrefix);
    if (doQuestPos != std::string::npos)
    {
        name = "do quest";
        std::string idStr = text.substr(doQuestPos + doQuestPrefix.length());
        try
        {
            questId = static_cast<uint32>(std::stoul(idStr));
        }
        catch (std::exception const&)
        {
            questId = 0;
        }
    }

    NewRpgStatus status = NewRpgInfo::StatusFromString(name);
    NewRpgInfo& info = botAI->rpgInfo;

    if (status == RPG_IDLE)
    {
        info.ChangeToIdle();
        WhisperStatusChange(owner, "IDLE");
        return true;
    }
    else if (status == RPG_REST)
    {
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WhisperStatusChange(owner, "REST");
        return true;
    }
    else if (status == RPG_WANDER_RANDOM)
    {
        info.ChangeToWanderRandom();
        WhisperStatusChange(owner, "WANDER_RANDOM");
        return true;
    }
    else if (status == RPG_WANDER_NPC)
    {
        info.ChangeToWanderNpc();
        WhisperStatusChange(owner, "WANDER_NPC");
        return true;
    }
    else if (status == RPG_GO_GRIND)
    {
        WorldPosition pos = SelectRandomGrindPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_grind_pos_error", "No grind position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoGrind(pos);
        WhisperStatusChange(owner, "GO_GRIND");
        return true;
    }
    else if (status == RPG_GO_CAMP)
    {
        WorldPosition pos = SelectRandomCampPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_camp_pos_error", "No camp position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoCamp(pos);
        WhisperStatusChange(owner, "GO_CAMP");
        return true;
    }
    else if (status == RPG_TRAVEL_FLIGHT)
    {
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        if (!SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_flight_path_error", "No flight path available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, std::move(path));
        WhisperStatusChange(owner, "TRAVEL_FLIGHT");
        return true;
    }
    else if (status == RPG_OUTDOOR_PVP)
    {
        info.ChangeToOutdoorPvp();
        WhisperStatusChange(owner, "OUTDOOR_PVP");
        return true;
    }
    else if (status == RPG_DO_QUEST)
    {
        if (!questId)
        {
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 qid = bot->GetQuestSlotQuestId(slot);
                if (!qid)
                    continue;
                std::vector<POIInfo> poi;
                if (GetQuestPOIPosAndObjectiveIdx(qid, poi, true))
                {
                    questId = qid;
                    break;
                }
            }
        }
        if (!questId)
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_quest_error", "No quest available; use 'do quest <id>'.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus questStatus = bot->GetQuestStatus(questId);
        if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_invalid_quest_error", "Invalid quest %quest_id",
                {{"%quest_id", std::to_string(questId)}});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoQuest(questId, quest);
        WhisperStatusChange(owner, "DO_QUEST " + std::to_string(questId));
        return true;
    }

    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "rpg_unknown_status_error",
        "Unknown rpg status. Options: idle, rest, wander random, wander npc, "
        "go grind, go camp, do quest [<id>], travel flight, outdoor pvp.", {});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
    return false;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP, RPG_VENDOR, RPG_MAILBOX, RPG_GATHER, RPG_TRAIN});

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            if (originalPos == WorldPosition())
            {
                LOG_DEBUG("playerbots", "[New RPG] {} in GO_GRIND with no destination, returning to idle",
                          bot->GetName());
                info.ChangeToIdle();
                return true;
            }
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            // Could not get there in time - pick something else rather than walking forever.
            if (info.HasStatusPersisted(statusGoGrindDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            if (originalPos == WorldPosition())
            {
                LOG_DEBUG("playerbots", "[New RPG] {} in GO_CAMP with no destination, returning to idle",
                          bot->GetName());
                info.ChangeToIdle();
                return true;
            }
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
            if (info.HasStatusPersisted(statusGoCampDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle();
                return true;
            }
            // Never cut a flight short; the cap is for a bot that cannot reach the flight master
            // at all, or whose flight master despawned between selection and arrival.
            if (!bot->IsInFlight() && info.HasStatusPersisted(statusTravelFlightDuration))
            {
                LOG_DEBUG("playerbots", "[New RPG] {} gave up travelling to flight master {}", bot->GetName(),
                          data.flightMasterEntry);
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAIN:
        {
            if (info.HasStatusPersisted(statusTrainDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_GATHER:
        {
            if (info.HasStatusPersisted(statusGatherDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_VENDOR:
        {
            if (info.HasStatusPersisted(statusVendorDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_MAILBOX:
        {
            if (info.HasStatusPersisted(statusMailboxDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // No npc can be found, switch to IDLE
        ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract();
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const* statusData = GetQuestStatusData(questId);
        if (!quest || !statusData)
        {
            // Quest template removed from the DB, or the quest is no longer in the bot's log.
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }

        QuestStatusData const& q_status = *statusData;
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
            data.poiAttempts = 0;
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        POIInfo const* picked = RandomElement(poiInfo);
        if (!picked)
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }

        float dx = picked->pos.x, dy = picked->pos.y;
        int32 objectiveIdx = picked->objectiveIdx;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const* statusData = GetQuestStatusData(questId);
        if (!quest || !statusData)
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }

        QuestStatusData const& q_status = *statusData;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // No progress after `poiStayTime` at this POI. That is usually a bad sample rather than
            // a broken quest, so try a different candidate point for the same objective first and
            // only give up once maxPoiAttempts of them have failed.
            if (++data.poiAttempts < maxPoiAttempts)
            {
                LOG_DEBUG("playerbots", "[New RPG] {} no progress on quest {} at POI {}/{}, trying another POI",
                          bot->GetName(), questId, data.poiAttempts, maxPoiAttempts);
                data.lastReachPOI = 0;
                data.pos = WorldPosition();
                return true;
            }

            sQuestBlacklistMgr.ReportFailure(questId);
            botAI->rpgStatistic.questAbandoned++;
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        data.poiAttempts = 0;
        return true;
    }

    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    const Quest* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true) || poiInfo.empty())
        {
            // can't find a poi pos to reward, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        // now we get the place to get rewarded - pick at random rather than always taking the
        // first candidate, so a turn-in that repeatedly fails gets a different approach point
        POIInfo const* picked = RandomElement(poiInfo);
        if (!picked)
        {
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }

        float dx = picked->pos.x, dy = picked->pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        // Only announce and count the completion once we actually have somewhere to hand it in.
        // Counting it before the lookup meant every failed lookup re-broadcast and re-incremented.
        if (!data.completionAnnounced)
        {
            BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
            botAI->rpgStatistic.questCompleted++;
            data.completionAnnounced = true;
            // Turn-in gets its own retry budget, independent of whatever the objective phase used.
            data.poiAttempts = 0;
        }

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // Could not hand the quest in here. Try another approach point for the turn-in before
        // writing the quest off - e.g. a POI polygon that spans a building will often sample a
        // spot the questgiver cannot be reached from.
        if (++data.poiAttempts < maxPoiAttempts)
        {
            LOG_DEBUG("playerbots", "[New RPG] {} could not turn in quest {} at POI {}/{}, trying another POI",
                      bot->GetName(), questId, data.poiAttempts, maxPoiAttempts);
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
            return true;
        }

        // e.g. Can not reward quest to gameobjects
        sQuestBlacklistMgr.ReportFailure(questId);
        botAI->rpgStatistic.questAbandoned++;
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

bool NewRpgVendorAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::Vendor>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;

    if (data.pos == WorldPosition())
    {
        info.ChangeToIdle();
        return true;
    }

    if (bot->GetDistance(data.pos) > INTERACTION_DISTANCE && !data.lastReach)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    if (!data.lastReach)
    {
        data.lastReach = getMSTime();
        return true;
    }

    if (!data.sold)
    {
        // "sell" and "repair" already handle finding the nearby vendor NPC and the packet exchange.
        botAI->DoSpecificAction("sell", Event(), true);
        botAI->DoSpecificAction("repair", Event(), true);
        data.sold = true;
        return true;
    }

    if (GetMSTimeDiffToNow(data.lastReach) >= vendorStayTime)
        info.ChangeToIdle();

    return true;
}

bool NewRpgMailboxAction::Execute(Event /*event*/)
{
    // Take money and items from every mail, regardless of who sent it. See the class comment for
    // why CheckMailAction cannot be reused: it drops anything not sent by a connected non-bot
    // player, which is every auction payment.
    //
    // Mirrors WorldSession::HandleMailTakeItem's persistence exactly. The first version of this
    // moved items into the bags in memory only -- no transaction, no Mail::RemoveItem, no
    // removedItems, no _SaveMail. The item therefore ended up in the bot's bags *and* still
    // attached to its mail row in the database, which is genuine item duplication: two owners for
    // one item_instance. It showed up as auction-won mail whose item was also sitting in the
    // winner's inventory.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    uint32 collected = 0;
    uint32 money = 0;
    std::vector<uint32> emptied;
    std::vector<uint32> takenItems;
    std::vector<Mail const*> partiallyTaken;

    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->state == MAIL_STATE_DELETED)
            continue;

        // Undelivered mail is not the bot's to take yet.
        if (mail->deliver_time > GameTime::GetGameTime().count())
            continue;

        bool changed = false;

        if (mail->money)
        {
            money += mail->money;
            bot->ModifyMoney(static_cast<int32>(mail->money));
            mail->money = 0;
            changed = true;
        }

        // Copied, because Mail::RemoveItem mutates the very vector being walked.
        MailItemInfoVec const attachments = mail->items;

        bool itemsPending = false;
        for (MailItemInfo const& att : attachments)
        {
            Item* item = bot->GetMItem(att.item_guid);
            if (!item)
                continue;

            ItemPosCountVec dest;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
            {
                // Leave it attached rather than destroying it; the bot will come back with space.
                itemsPending = true;
                continue;
            }

            mail->RemoveItem(att.item_guid);
            bot->RemoveMItem(att.item_guid);
            takenItems.push_back(att.item_guid);

            // Without this the item cannot be removed from the bags later on.
            item->SetState(ITEM_UNCHANGED);
            bot->MoveItemToInventory(dest, item, true);

            ++collected;
            changed = true;
        }

        if (itemsPending)
        {
            if (changed)
            {
                mail->state = MAIL_STATE_CHANGED;
                partiallyTaken.push_back(mail);
            }
            continue;
        }

        mail->state = MAIL_STATE_DELETED;
        emptied.push_back(mail->messageID);
    }

    // Player::_SaveMail would do all of this, but it is protected and only WorldSession may call
    // it, so the statements are issued directly -- the same approach CheckMailAction already takes.
    for (uint32 itemGuid : takenItems)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM);
        stmt->SetData(0, itemGuid);
        trans->Append(stmt);
    }

    for (uint32 id : emptied)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
        stmt->SetData(0, id);
        trans->Append(stmt);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
        stmt->SetData(0, id);
        trans->Append(stmt);
    }

    // Mail that gave up its money but still holds items the bags had no room for: persist the
    // zeroed money so a restart cannot pay the bot twice.
    for (Mail const* mail : partiallyTaken)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_MAIL);
        stmt->SetData(0, uint8(!mail->items.empty()));
        stmt->SetData(1, uint32(mail->expire_time));
        stmt->SetData(2, uint32(mail->deliver_time));
        stmt->SetData(3, mail->money);
        stmt->SetData(4, mail->COD);
        stmt->SetData(5, uint8(mail->checked));
        stmt->SetData(6, mail->messageID);
        trans->Append(stmt);
    }

    bot->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    for (uint32 id : emptied)
    {
        bot->SendMailResult(id, MAIL_DELETED, MAIL_OK);
        bot->RemoveMail(id);
    }

    if (money || collected)
        LOG_DEBUG("playerbots", "[Logistics] {} collected {} copper and {} item(s) from mail",
                  bot->GetName(), money, collected);

    botAI->rpgInfo.ChangeToIdle();
    return true;
}

bool NewRpgGatherAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::Gather>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;

    GatherRouteMgr::Route const* route = sGatherRouteMgr.PickRoute(bot, data.zoneId);
    if (!route || route->nodes.empty())
    {
        info.ChangeToIdle();
        return true;
    }

    // Wandered out of the zone the route belongs to - the route is no longer reachable.
    if (bot->GetZoneId() != data.zoneId)
    {
        info.ChangeToIdle();
        return true;
    }

    if (data.routeIndex >= route->nodes.size())
    {
        LOG_DEBUG("playerbots", "[Gather] {} completed a {}-node route in zone {} ({} nodes visited)",
                  bot->GetName(), route->nodes.size(), data.zoneId, data.nodesVisited);
        info.ChangeToIdle();
        return true;
    }

    GatherRouteMgr::Node const& node = route->nodes[data.routeIndex];
    if (data.pos == WorldPosition())
        data.pos = WorldPosition(node.mapId, node.x, node.y, node.z);

    // Arrive within DETECTION range, not interaction range.
    //
    // This previously required INTERACTION_DISTANCE (5.5 yards) of the waypoint. That is the wrong
    // measure entirely: the bot never needs to stand on the node, it needs the node to be visible
    // to the `gather` strategy, which scans to AiPlayerbot.SightDistance (100 yards) and whose loot
    // action handles the final approach within LootDistance (15). Worse, a waypoint is a cluster
    // centroid - an averaged coordinate that may land inside a rock or mid-air - so 5.5 yards was
    // frequently unreachable and the bot burned its entire gather window on a single waypoint
    // without ever advancing. Measured: 20 bots gathering for 35 minutes produced zero items.
    if (bot->GetDistance(data.pos) > gatherArrivalDistance && !data.lastReach)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    if (!data.lastReach)
        LOG_DEBUG("playerbots", "[Gather] {} reached waypoint {}/{} in zone {} (dist {:.1f})", bot->GetName(),
                  data.routeIndex + 1, route->nodes.size(), data.zoneId, bot->GetDistance(data.pos));

    if (!data.lastReach)
    {
        data.lastReach = getMSTime();
        return true;
    }

    // Linger briefly so the `gather` strategy can act, then move to the next node whether or not
    // the node was still there - another bot or a player may have taken it.
    if (GetMSTimeDiffToNow(data.lastReach) >= nodeStayTime)
    {
        data.routeIndex++;
        data.nodesVisited++;
        data.lastReach = 0;
        data.pos = WorldPosition();
    }

    return true;
}

bool NewRpgTrainAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::Train>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;

    if (data.pos == WorldPosition())
    {
        info.ChangeToIdle();
        return true;
    }

    if (bot->GetDistance(data.pos) > INTERACTION_DISTANCE && !data.lastReach)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    if (!data.lastReach)
    {
        data.lastReach = getMSTime();
        return true;
    }

    if (!data.trained)
    {
        // "trainer" reads the bot's current target, so select the trainer first.
        Unit* trainer = ObjectAccessor::GetUnit(*bot, data.trainerGuid);
        if (!trainer)
        {
            info.ChangeToIdle();
            return true;
        }

        bot->SetSelection(data.trainerGuid);
        botAI->DoSpecificAction("trainer", Event(), true);
        data.trained = true;
        return true;
    }

    if (GetMSTimeDiffToNow(data.lastReach) >= trainerStayTime)
        info.ChangeToIdle();

    return true;
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle();
        return true;
    }
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster);

    std::vector<uint32> nodes = data.path;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes[0], nodes[nodes.size() - 1]);
        info.ChangeToIdle();
        return true;
    }
    return true;
}
