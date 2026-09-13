/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGACTION_H
#define PLAYERBOTS_NEWRPGACTION_H

#include "Duration.h"
#include "MovementActions.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"
#include <string>

class Player;

class TellRpgStatusAction : public NewRpgBaseAction
{
public:
    TellRpgStatusAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "rpg status") {}

    bool Execute(Event event) override;

private:
    static constexpr char const* RPG_STATUS_CHANGED_KEY = "rpg_status_changed";
    static constexpr char const* RPG_STATUS_CHANGED_DEFAULT = "rpg status -> %status";

    void WhisperStatusChange(Player* owner, std::string const& statusName);
};

class StartRpgDoQuestAction : public Action
{
public:
    StartRpgDoQuestAction(PlayerbotAI* botAI) : Action(botAI, "start rpg do quest") {}

    bool Execute(Event event) override;
};

class NewRpgStatusUpdateAction : public NewRpgBaseAction
{
public:
    NewRpgStatusUpdateAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg status update")
    {
        // int statusCount = RPG_STATUS_END - 1;

        // transitionMat.resize(statusCount, std::vector<int>(statusCount, 0));

        // transitionMat[RPG_IDLE][RPG_GO_GRIND] = 20;
        // transitionMat[RPG_IDLE][RPG_GO_CAMP] = 15;
        // transitionMat[RPG_IDLE][RPG_WANDER_NPC] = 30;
        // transitionMat[RPG_IDLE][RPG_DO_QUEST] = 35;
    }
    bool Execute(Event event) override;

protected:
    /// May a completed quest's hand-in interrupt the activity the bot is running right now?
    bool ShouldPreemptForTurnIn(NewRpgStatus status);

    // static NewRpgStatusTransitionProb transitionMat;
    const int32 statusWanderNpcDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusWanderRandomDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusRestDuration = 30 * IN_MILLISECONDS ;
    const int32 statusDoQuestDuration = 30 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusOutDoorPvPDuration = HOUR * IN_MILLISECONDS ;
    // GO_GRIND, GO_CAMP and TRAVEL_FLIGHT used to leave their status only on arrival, so a bot
    // that could never reach its destination (or whose flight master despawned) stayed in that
    // status forever. Every other status already had a cap; these are theirs.
    const int32 statusGoGrindDuration = 10 * MINUTE * IN_MILLISECONDS;
    const int32 statusGoCampDuration = 10 * MINUTE * IN_MILLISECONDS;
    const int32 statusTravelFlightDuration = 15 * MINUTE * IN_MILLISECONDS;
    const int32 statusVendorDuration = 5 * MINUTE * IN_MILLISECONDS;
    const int32 statusMailboxDuration = 30 * IN_MILLISECONDS;
    // 25, not 15. The window covers travelling to the route as well as working it, and a bot that
    // spends four minutes walking to the hills should still get a useful shift out of the trip.
    const int32 statusGatherDuration = 25 * MINUTE * IN_MILLISECONDS;
    const int32 statusTrainDuration = 5 * MINUTE * IN_MILLISECONDS;
    // A cast plus its wait is about twenty seconds, so this is roughly thirty casts -- a session
    // long enough to be worth the walk to the water and short enough that a bot which picked a spot
    // it cannot actually fish is not stuck there for the rest of the evening.
    const int32 statusFishDuration = 10 * MINUTE * IN_MILLISECONDS;
};

class NewRpgGoGrindAction : public NewRpgBaseAction
{
public:
    NewRpgGoGrindAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go grind") {}
    bool Execute(Event event) override;
};

class NewRpgGoCampAction : public NewRpgBaseAction
{
public:
    NewRpgGoCampAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go camp") {}
    bool Execute(Event event) override;
};

class NewRpgWanderRandomAction : public NewRpgBaseAction
{
public:
    NewRpgWanderRandomAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg wander random") {}
    bool Execute(Event event) override;
};

class NewRpgWanderNpcAction : public NewRpgBaseAction
{
public:
    NewRpgWanderNpcAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg move npcs") {}
    bool Execute(Event event) override;

    const uint32 npcStayTime = 8 * 1000;
};

class NewRpgDoQuestAction : public NewRpgBaseAction
{
public:
    NewRpgDoQuestAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do quest") {}
    bool Execute(Event event) override;

protected:
    bool DoIncompleteQuest(NewRpgInfo::DoQuest& data);
    bool DoCompletedQuest(NewRpgInfo::DoQuest& data);

    const uint32 poiStayTime = 5 * 60 * 1000;
};

/// Travels to a vendor to offload what AutoVendorJunk cannot: white-quality items, and anything
/// else the classifier marks as vendor fodder above the auto-sell quality threshold. Also repairs.
///
/// AutoVendorJunk deliberately handles only greys with no travel (R4.1), so whites still accumulate
/// and need a real vendor trip - this is that trip.
class NewRpgVendorAction : public NewRpgBaseAction
{
public:
    NewRpgVendorAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg vendor") {}
    bool Execute(Event event) override;

    const uint32 vendorStayTime = 5 * 1000;
};

/// Collects mail without visiting a mailbox, in the same spirit as posting to the auction house
/// from anywhere.
///
/// This does NOT reuse CheckMailAction. That action skips any mail whose sender is not a currently
/// connected non-bot player:
///     Player* owner = ObjectAccessor::FindConnectedPlayer(mail->sender);
///     if (!owner) continue;
/// Auction proceeds are sent by the auction house, not by a connected player, so every auction
/// payment would be silently skipped. CheckMailAction is for player gifts and guild tasks; the
/// economy needs its own path.
class NewRpgMailboxAction : public NewRpgBaseAction
{
public:
    NewRpgMailboxAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg mailbox") {}
    bool Execute(Event event) override;
};

/// Walks a precomputed gathering route, node to node.
///
/// Deliberately does NOT harvest anything itself. The existing `gather` strategy
/// (AddGatheringLootAction + LootObjectStack) already picks up any node that comes within loot
/// range, and it works. All that was ever missing was a reason to be standing next to one, which
/// is what this provides.
class NewRpgGatherAction : public NewRpgBaseAction
{
public:
    NewRpgGatherAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg gather") {}
    bool Execute(Event event) override;

    // Long enough for the `gather` strategy to notice the node, cast, and loot it.
    const uint32 nodeStayTime = 10 * 1000;
    // Well inside AiPlayerbot.SightDistance (100) so the whole cluster is detectable on arrival,
    // while still close enough that the loot action's approach is short.
    const float gatherArrivalDistance = 40.0f;
};

/// Travels to a trainer and learns whatever is available, profession ranks included.
///
/// This exists because nothing else raises a bot past Apprentice. PlayerbotFactory grants the
/// starter spell for each profession and stops there, so without training every bot is capped at
/// skill 75 forever - meaning the auction house would only ever see Peacebloom and Copper Ore no
/// matter how much gathering happens.
///
/// Reuses the existing "trainer" action rather than reimplementing the learn logic: it already
/// walks sObjectMgr->GetTrainer() and handles tradeskill trainers. All that was missing was a bot
/// deciding to go and stand in front of one.
class NewRpgTrainAction : public NewRpgBaseAction
{
public:
    NewRpgTrainAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg train") {}
    bool Execute(Event event) override;

    const uint32 trainerStayTime = 5 * 1000;
};

/**
 * Fishes, on the bot's own initiative.
 *
 * Every mechanical piece of fishing already existed and worked: EquipFishingPoleAction finds or
 * conjures a pole, MoveNearWaterAction finds a shoreline to stand on, FishingAction turns to face
 * the water and casts, UseBobberAction loots the catch. All of it was reachable only through
 * MasterFishingStrategy -- which is switched on when a *human* starts fishing nearby. With no one
 * to copy, a bot with 400 fishing never once cast a line.
 *
 * So this adds no fishing mechanics. It delegates to those same actions, in the order the strategy
 * would have run them, and its whole contribution is that a bot standing near water sometimes
 * decides to fish.
 *
 * What it catches needs no special handling either: raw fish are cooking reagents, so the P10.3
 * skill-up pass cooks them, and the surplus is trade goods the economy already knows how to sell.
 */
class NewRpgFishAction : public NewRpgBaseAction
{
public:
    NewRpgFishAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg fish") {}
    bool Execute(Event event) override;
};

class NewRpgTravelFlightAction : public NewRpgBaseAction
{
public:
    NewRpgTravelFlightAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg travel flight") {}
    bool Execute(Event event) override;
};

#endif
