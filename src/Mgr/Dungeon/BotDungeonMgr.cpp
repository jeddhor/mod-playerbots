/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotDungeonMgr.h"

#include "Formations.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Map.h"
#include "Timer.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

namespace
{
    // The formation followers are put into while a bot leads. "melee" would also target the group
    // leader, but its name promises melee range to anyone reading a `.formation ?`, and a healer
    // told it is in a melee formation is a confusing thing to explain later.
    constexpr char const* DUNGEON_FORMATION = "dungeon";
}

bool BotDungeonMgr::RepairStaleLfgGroup(Player* player)
{
    Group* group = player ? player->GetGroup() : nullptr;
    if (!group || !group->isLFGGroup())
        return false;

    // Only the unambiguous case: no dungeon and no state at all. A group queued, in a role check, in a
    // proposal, in its dungeon or finished with it always has one or the other.
    ObjectGuid const gguid = group->GetGUID();
    if (sLFGMgr->GetDungeon(gguid) || sLFGMgr->GetState(gguid) != lfg::LFG_STATE_NONE)
        return false;

    LOG_INFO("playerbots", "[Dungeon] Group {} led by {} was a dungeon finder group with no dungeon; made it an ordinary group",
             gguid.GetCounter(), player->GetName());

    group->ConvertFromLFG();
    return true;
}

bool BotDungeonMgr::CheckRunLimits(Player* bot, uint32 now)
{
    ObjectGuid const guid = bot->GetGUID();
    Map* map = bot->FindMap();

    // Dungeons and raids only. A battleground ends on its own terms.
    if (!map || !map->IsDungeon() || bot->InBattleground())
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _runs.erase(guid);
        return false;
    }

    Group* group = bot->GetGroup();

    // Never on a group a person is in. A human, their alts and their self bot are playing the dungeon
    // at whatever pace they like; pulling their party out from under them because it took a while is
    // the opposite of what anyone wants. Only an all-random-bot run is abandoned.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (group)
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (!GET_PLAYERBOT_AI(member) || IsSelfBot(member) || !sRandomPlayerbotMgr.IsRandomBot(member))
                    return false;

    // Progress is fighting. Anyone in the group in combat means the run is still doing something; a
    // run where nobody has fought for a while has stalled, whatever the clock says.
    bool fighting = bot->IsInCombat();
    if (group && !fighting)
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource(); member && member->IsInCombat())
            {
                fighting = true;
                break;
            }

    Run run;
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        Run& stored = _runs[guid];
        if (stored.mapId != map->GetId() || stored.instanceId != map->GetInstanceId())
        {
            stored.mapId = map->GetId();
            stored.instanceId = map->GetInstanceId();
            stored.enteredMs = now;
            stored.lastProgressMs = now;
        }

        if (fighting)
            stored.lastProgressMs = now;

        run = stored;
    }

    uint32 const inside = getMSTimeDiff(run.enteredMs, now);
    uint32 const idle = getMSTimeDiff(run.lastProgressMs, now);
    bool const timedOut = inside > sPlayerbotAIConfig.dungeonMaxMinutes * MINUTE * IN_MILLISECONDS;
    bool const stalled = idle > sPlayerbotAIConfig.dungeonStallMinutes * MINUTE * IN_MILLISECONDS;

    if (!timedOut && !stalled)
        return false;

    LOG_INFO("playerbots", "[Dungeon] {} gives up on map {} after {} min inside ({} min without a fight): {}",
             bot->GetName(), map->GetId(), inside / MINUTE / IN_MILLISECONDS, idle / MINUTE / IN_MILLISECONDS,
             timedOut ? "run took too long" : "run stalled");

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _runs.erase(guid);
        if (timedOut)
            ++_runsAbandonedTimeout;
        else
            ++_runsAbandonedStalled;
    }

    // Out the way the dungeon finder brought them in if it did, so the queue's own bookkeeping ends
    // cleanly; otherwise back into the world at a level-appropriate spot.
    bool const viaLfg = sLFGMgr->inLfgDungeonMap(guid, map->GetId(), map->GetDifficulty());

    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        if (bot->GetGroup())
            botAI->LeaveOrDisbandGroup();

    if (viaLfg)
        sLFGMgr->TeleportPlayer(bot, true);
    else
        sRandomPlayerbotMgr.RandomTeleportForLevel(bot);

    return true;
}

void BotDungeonMgr::Update(Player* bot, uint32 diff)
{
    if (!bot || !bot->IsInWorld())
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    if (!sPlayerbotAIConfig.dungeonAutopilotEnabled)
        return;

    ObjectGuid const guid = bot->GetGUID();
    uint32 const now = getMSTime();

    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        auto const itr = _nextCheckMs.find(guid);
        if (itr != _nextCheckMs.end() && now < itr->second)
            return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _nextCheckMs[guid] = now + sPlayerbotAIConfig.dungeonAutopilotIntervalMs;
    }

    RepairStaleLfgGroup(bot);

    if (CheckRunLimits(bot, now))
        return;

    // Polled rather than driven by group and map events. Entering an instance, a leadership
    // handover, a disband and a wipe-and-release all change the answer, and catching each of them
    // separately means one missed hook leaves a group standing still with no way to recover. A
    // poll re-derives the truth every couple of seconds whatever happened.
    Mode const want = DecideMode(bot);

    // Worked on a copy rather than a reference into the map. Apply() below can take a while --
    // ResetStrategies rebuilds every engine -- and holding a pointer into a container another
    // thread may rehash or erase from for that long is the kind of thing that works until the
    // realm is busy.
    State state;
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        if (auto const itr = _state.find(guid); itr != _state.end())
            state = itr->second;
    }

    if (state.mode == want)
        return;

    Mode const previous = state.mode;
    Apply(bot, want, state);

    std::unique_lock<std::shared_mutex> lock(_mutex);
    state.mode = want;
    state.sinceMs = now;
    _state[guid] = state;

    if (previous == Mode::Off && want != Mode::Off)
        ++_engaged;
    else if (previous != Mode::Off && want == Mode::Off)
        ++_disengaged;

    if (want == Mode::Leading)
        ++_leaders;
    else if (want == Mode::Following)
        ++_followers;

    (void)diff;
}

BotDungeonMgr::Mode BotDungeonMgr::DecideMode(Player* bot)
{
    // A battleground is instanceable but has its own strategies, and dropping dungeon navigation
    // on top of them would fight the battleground AI for the same bot.
    if (bot->InBattleground())
        return Mode::Off;

    Map* map = bot->FindMap();
    if (!map || !map->IsDungeon())
        return Mode::Off;

    Group* group = bot->GetGroup();
    if (!group)
        return Mode::Off;

    ObjectGuid const leaderGuid = group->GetLeaderGUID();
    Player* leader = ObjectAccessor::FindPlayer(leaderGuid);
    if (!leader || !leader->IsInWorld())
        return Mode::Off;

    // Only a bot leader means "you drive". A human holding lead is leading, and the group following
    // them is the behaviour that already works -- taking navigation away from a person who never
    // asked would be a far worse bug than the one this fixes.
    if (!GET_PLAYERBOT_AI(leader))
        return Mode::Off;

    // A leader on another map cannot be followed to a pull, and a bot that tries walks into a wall
    // until something teleports it.
    if (leader->GetMapId() != bot->GetMapId() || leader->GetInstanceId() != bot->GetInstanceId())
        return Mode::Off;

    return leaderGuid == bot->GetGUID() ? Mode::Leading : Mode::Following;
}

void BotDungeonMgr::Apply(Player* bot, Mode mode, State& state)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    switch (mode)
    {
        case Mode::Leading:
            // `new rpg` is where P13.4's pull selection lives, and `follow` has to go: a leader that
            // is also trying to hold formation on somebody spends every tick undoing its own
            // movement.
            if (state.savedFormation.empty())
                state.savedFormation = CurrentFormation(bot);

            botAI->ChangeStrategy("-follow,+new rpg,+grind", BOT_STATE_NON_COMBAT);
            LOG_INFO("playerbots", "[Dungeon] {} takes the lead on map {}; group runs itself",
                     bot->GetName(), bot->GetMapId());
            break;

        case Mode::Following:
            if (state.savedFormation.empty())
                state.savedFormation = CurrentFormation(bot);

            // Navigation is removed as well as follow being added. Five bots each choosing their own
            // pull is not a group clearing a dungeon, and the leader is the one making that choice.
            botAI->ChangeStrategy("+follow,-new rpg", BOT_STATE_NON_COMBAT);
            SetFormation(bot, DUNGEON_FORMATION);
            break;

        case Mode::Off:
            // Recompute from the factory rather than inverting the strings above: what a bot should
            // carry outside a dungeon depends on whether it has a master, whether it is a random
            // bot and where it is, and AiFactory is the one place that knows all of that.
            botAI->ResetStrategies();

            if (!state.savedFormation.empty())
            {
                SetFormation(bot, state.savedFormation);
                state.savedFormation.clear();
            }

            if (botAI->GetMaster())
                botAI->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
            break;
    }
}

void BotDungeonMgr::SetFormation(Player* bot, std::string const& name)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    if (FormationValue* value =
            dynamic_cast<FormationValue*>(botAI->GetAiObjectContext()->GetValue<Formation*>("formation")))
    {
        value->Load(name);
    }
}

std::string BotDungeonMgr::CurrentFormation(Player* bot) const
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return "";

    if (Formation* formation = botAI->GetAiObjectContext()->GetValue<Formation*>("formation")->Get())
        return formation->getName();

    return "";
}

void BotDungeonMgr::Forget(ObjectGuid guid)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _state.erase(guid);
    _nextCheckMs.erase(guid);
}

std::string BotDungeonMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);

    uint32 leading = 0;
    uint32 following = 0;
    for (auto const& entry : _state)
    {
        if (entry.second.mode == Mode::Leading)
            ++leading;
        else if (entry.second.mode == Mode::Following)
            ++following;
    }

    std::ostringstream out;
    out << "Dungeon autopilot: " << leading << " leading, " << following << " following now; "
        << _engaged << " engaged, " << _disengaged << " disengaged, "
        << _leaders << " lead handovers, " << _followers << " follower switches; runs abandoned: "
        << _runsAbandonedTimeout << " too long, " << _runsAbandonedStalled << " stalled";
    return out.str();
}
