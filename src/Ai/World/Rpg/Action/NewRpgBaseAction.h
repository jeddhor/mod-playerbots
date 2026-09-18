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

    /**
     * The objective's real height, when it was taken from a spawn rather than from the map pin.
     *
     * quest_poi_points stores X and Y only -- there is no Z column -- so a destination derived from
     * it has to guess the height from the terrain under the pin. That guess is wrong wherever the
     * objective is not at ground level: a bot sent to Felendren the Banished patrols beneath the
     * tower its targets stand on, and one sent to Skull Rock walks the hillside above the cave.
     */
    float z{0.0f};
    bool hasZ{false};
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

    /// Snap a destination onto the walkable surface at its x/y. False when there is none, in which
    /// case the destination is not somewhere a bot can stand and should not be walked to.
    static bool ResolveTeleportGround(Player* bot, WorldPosition& dest);

    /**
     * P11.8 -- step off a ledge on purpose, when that is the only way down.
     *
     * A bot on a raised platform has no representation of "fall" as a way to travel: the navmesh has
     * no edge from the platform to the ground, correctly, because there is no walkable surface
     * between them. So pathing reports no route and the bot runs the platform until its activity
     * window expires. It is not judging the drop unsafe; it cannot see the drop at all.
     *
     * This is therefore a missing move rather than a cost function to tune. Damage is estimated with
     * the core's own fall equation, because a threshold that disagrees with what actually happens
     * would either strand bots or kill them.
     *
     * Distinct from the stuck teleport below it and from P7.24's out-of-world recovery. Three
     * different things end with a bot somewhere new; a log that cannot tell them apart hides
     * whichever one starts misbehaving.
     */
    bool TryDeliberateDrop(WorldPosition const& dest);

    /**
     * 4k -- get out of the building first.
     *
     * A bot inside an inn has no notion of a door. Pathing is asked for a route to somewhere across
     * the zone, and the only leg it can offer first walks *away* from that destination -- out through
     * the doorway -- so the "is this endpoint closer to where I am going" test rejects it and the
     * fallback samples the cone toward the destination, which is the wall. An operator watched their
     * character walk in and out of the Lakeshire inn seven times doing exactly that.
     *
     * So when a bot indoors cannot get a route accepted, the destination becomes the nearest outdoor
     * spot it can actually path to, and the real walk is attempted again from there. Only outside
     * instances: in a dungeon every point is indoors, and hunting for an outdoor one would replace
     * one wrong walk with another.
     */
    bool MoveOutOfBuilding(WorldPosition const& dest);

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

    /// Work-order tier for a held quest: complete, then green, yellow, grey, orange, red.
    int32 QuestWorkPriority(uint32 questId, Quest const* quest);

    /// A completed quest whose hand-in is within the turn-in priority distance, or 0.
    /// P13.4 -- the next pull inside an instance, or an empty position when there is none.
    WorldPosition SelectDungeonPullPos();

    /// Nearest reachable, unengaged hostile anywhere in the instance -- where a leader goes when the
    /// pull search finds nothing close by.
    WorldPosition SelectDungeonAdvancePos();

    /// Whether everyone in the run is fit to take the next pull: alive, out of combat, healthy, and
    /// the healers topped up on mana.
    bool DungeonGroupReady();
    uint32 _lastReadyLogMs{0};
    /// Talk to a nearby progression NPC when a dungeon group has nothing left to pull.
    bool TryDungeonGossip();

    /**
     * Is this area somewhere the other faction owns?
     *
     * Nothing in destination selection asked this, so a level 13 blood elf walked out of a gather
     * route in Elwynn, through the gates of Stormwind, and started trading blows with a level 22
     * guard. She was not doing anything wrong by her own rules -- the guard struck first, and
     * fighting back bypasses the level filter, correctly. The mistake was made long before, when
     * something chose a destination in a city her faction cannot enter.
     */
    /// Whose side an area belongs to, if anyone's. Most areas belong to nobody.
    static bool IsEnemyArea(Player* bot, uint32 areaOrZoneId);

    /// An enemy capital specifically. Always avoided, whatever the territory policy says.
    static bool IsEnemyCapital(Player* bot, uint32 areaOrZoneId);

    /**
     * Should a destination here be passed over?
     *
     * Answers the whole policy, including the roll when enemy territory is merely discouraged, so
     * every caller that picks a destination asks one question and gets one answer.
     */
    static bool ShouldAvoidArea(Player* bot, uint32 areaOrZoneId);

    /// Is this activity permitted for this bot -- base weight, with any archetype override?
    bool IsRpgStatusPermitted(NewRpgStatus status);

    uint32 FindNearbyTurnIn();

    /// The turn-in this bot has committed to, held until the quest leaves its log.
    uint32 _committedTurnIn{0};

    /// Real spawn positions for an objective's creature or object, nearest first. Empty if unknown.
    bool AddSpawnCandidates(Quest const* quest, int32 objectiveIdx, std::vector<POIInfo>& poiInfo);
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
    //
    // A floor under the five attempts, not a second requirement on top of them. At 90 seconds it was
    // the binding one: attempts land roughly every ten seconds, so a self bot walking in and out of the
    // Lakeshire inn reached 5/5 at 45 seconds and carried on to 8/5 before recovery fired, which reads
    // on the Inspector as the threshold simply being ignored. The floor only matters when legs are
    // very short and attempts pile up in a few seconds.
    const uint32 stuckTime = 45 * 1000;

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
