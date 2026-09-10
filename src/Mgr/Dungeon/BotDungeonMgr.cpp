/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotDungeonMgr.h"

#include "Formations.h"
#include "Group.h"
#include "Map.h"
#include "Timer.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

namespace
{
    // The formation followers are put into while a bot leads. "melee" would also target the group
    // leader, but its name promises melee range to anyone reading a `.formation ?`, and a healer
    // told it is in a melee formation is a confusing thing to explain later.
    constexpr char const* DUNGEON_FORMATION = "dungeon";
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
        << _leaders << " lead handovers, " << _followers << " follower switches";
    return out.str();
}
