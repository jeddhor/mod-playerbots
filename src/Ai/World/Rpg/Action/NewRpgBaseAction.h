/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGBASEACTION_H
#define PLAYERBOTS_NEWRPGBASEACTION_H

#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "Timer.h"
#include "TravelMgr.h"

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
};

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

protected:
    /* MOVEMENT RELATED */
    bool MoveFarTo(WorldPosition dest);
    bool MoveWorldObjectTo(ObjectGuid guid, float distance = INTERACTION_DISTANCE);
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, WorldObject* center = nullptr);
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    /* QUEST RELATED CHECK */
    /// Safe replacement for `bot->getQuestStatusMap().at(questId)`, which throws std::out_of_range
    /// for a quest that is not in the bot's log. Returns nullptr instead so callers can bail out.
    QuestStatusData const* GetQuestStatusData(uint32 questId) const;
    ObjectGuid ChooseNpcOrGameObjectToInteract(bool questgiverOnly = false, float distanceLimit = 0.0f);
    WorldObject* FindNearestQuestGiver(GuidVector const& candidates, float distanceLimit);
    bool HasQuestToAcceptOrReward(WorldObject* object);
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool CanInteractWithQuestGiver(Object* questGiver);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);
    bool IsQuestWorthDoing(Quest const* quest);
    bool IsQuestCapableDoing(Quest const* quest);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();
    /// How much this bot wants to keep `questId`. Higher is better; the quest-log tidy-up drops
    /// the lowest scorers. A null `quest` (template missing from the DB) scores lowest of all.
    float ScoreQuestForKeeping(uint32 questId, Quest const* quest);

    /**
     * How well a quest's objectives suit a bot, independent of level or progress.
     *
     * Kill-N and collect-N are the two shapes the AI can actually drive: find the creature, kill
     * it, loot it. Escorts, timed runs and scripted events are where a bot stalls, holds a quest
     * slot until it is dropped, and gets the quest blacklisted for a reason that was never the
     * quest's fault. Of 9464 quests, 6584 are kill/collect and 2880 are not.
     */
    float ScoreQuestObjectiveShape(uint32 questId, Quest const* quest);

    /**
     * Turn in quests that have gone grey, without walking back to the giver.
     *
     * A grey quest grants no experience, so completing it for free costs the realm nothing in
     * balance terms, and it is strictly better than the alternatives: dropping it throws away the
     * work, and carrying it wastes a log slot the bot needs for quests that still pay.
     *
     * This matters more than it looks for era-capped bots. A bot with experience disabled at 60 or
     * 70 never outgrows its own bracket, so everything below it is permanently grey -- without this
     * those quests would accumulate in the log forever.
     *
     * @return number of quests completed this pass.
     */
    uint32 AutoCompleteTrivialQuests();

    /**
     * Teleport an ungrouped bot to a distant quest turn-in it would otherwise spend the whole
     * activity walking to.
     *
     * Kept separate from MoveFarTo's stuck recovery on purpose, and logged under its own tag. The
     * two look identical in a log -- both end with a bot somewhere it was not a moment ago -- and
     * conflating them would hide a rising stuck-rate behind deliberate travel.
     *
     * Never for a grouped bot: vanishing mid-quest is exactly the behaviour that makes a party bot
     * feel broken, and a grouped bot has company that would be left behind.
     */
    bool TeleportToDistantTurnIn(uint32 questId, WorldPosition const& pos);
    void DropQuest(uint16 slot, uint32 questId, Quest const* quest);

protected:
    bool GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete = false);
    /// Turns one quest POI polygon into up to `poiSamplesPerArea` standable candidate points and
    /// appends the ones that pass the cheap terrain checks to `poiInfo`.
    void AddPoiCandidates(QuestPOI const& qPoi, std::vector<POIInfo>& poiInfo);
    /// True when a vendor trip would actually accomplish something: gear needing repair, or items
    /// the classifier calls vendor fodder that AutoVendorJunk will not sell without travel.
    bool HasVendorBusiness();
    /// True when a profession is at its rank cap, i.e. the next rank is what unblocks progress.
    bool HasTrainingBusiness();
    WorldPosition SelectNearestTrainerPos(ObjectGuid& trainerGuid);
    WorldPosition SelectNearestVendorPos();
    static WorldPosition SelectRandomGrindPos(Player* bot);
    static WorldPosition SelectRandomCampPos(Player* bot);
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path);
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus);
    bool CheckRpgStatusAvailable(NewRpgStatus status);

protected:
    /* FOR MOVE FAR */
    const float pathFinderDis = 70.0f;
    // Time without real progress toward dest before MoveFarTo
    // falls back to teleport recovery. Kept short enough that a
    // bot truly oscillating around an unreachable destination
    // (mmap returning non-progressing partial paths, or NOPATH +
    // cone fallback wandering) doesn't spin for 5 minutes before
    // the teleport fires, but long enough that a genuine long
    // walk that is slowly making progress never triggers it.
    const uint32 stuckTime = 90 * 1000;

    /* FOR QUEST POI SELECTION */
    // How many standable points to offer per POI polygon. More candidates means a better chance
    // that at least one of them sits on the actual objective rather than in a lake or a cliff face.
    static constexpr uint32 poiSamplesPerArea = 4;
    /* HOUSEKEEPING THROTTLES */
    // SearchQuestGiverAndAcceptOrReward ran at the top of GoGrind, GoCamp, WanderRandom and DoQuest
    // on every tick. Each run read two values that sweep 150 yards of grid and then called
    // Player::PrepareQuestMenu - a DB-backed, side-effecting menu build - per candidate. In a
    // capital that was dozens of PrepareQuestMenu calls per bot per tick.
    static constexpr uint32 questGiverSearchInterval = 2 * IN_MILLISECONDS;
    // OrganizeQuestLog walks 25 quest slots scoring each one. It only needs to run when the log is
    // nearly full, which changes on the timescale of quest pickups, not ticks.
    static constexpr uint32 questLogOrganizeInterval = 30 * IN_MILLISECONDS;

    // How many POIs a bot will sit at without making progress before it gives up on the quest.
    // The old code blacklisted the quest after the very first unproductive POI, so a single bad
    // sample permanently cost the bot a perfectly good quest.
    static constexpr uint8 maxPoiAttempts = 3;
};

#endif
