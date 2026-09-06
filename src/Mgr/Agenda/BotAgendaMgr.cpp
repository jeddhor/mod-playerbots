/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotAgendaMgr.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "Playerbots.h"
#include "QueryResult.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "SharedDefines.h"
#include "Timer.h"

#include <algorithm>

namespace
{
    constexpr uint8 GOAL_COUNT = static_cast<uint8>(BotGoalType::Max);

    /**
     * How much each goal cares about each activity. 1.0 is indifference.
     *
     * Kept as a plain table rather than a behaviour tree or utility framework on purpose: it can be
     * read at a glance, printed by a chat command, and reasoned about when a bot does something
     * unexpected. Anything cleverer would be harder to debug than the behaviour it produces.
     */
    float const AFFINITY[GOAL_COUNT][RPG_STATUS_END] = {
        // IDLE GRIND CAMP WANDR NPC  QUEST FLIGHT REST PVP  VENDOR MAIL GATHER TRAIN
        /* LevelUp            */ {1.0f, 2.0f, 1.0f, 0.8f, 1.0f, 2.5f, 1.2f, 0.6f, 0.8f, 1.0f, 1.0f, 0.8f, 1.0f},
        /* EarnGold           */ {1.0f, 1.5f, 1.0f, 0.8f, 1.0f, 1.2f, 1.0f, 0.6f, 0.6f, 1.3f, 1.4f, 2.5f, 1.0f},
        /* AcquireGear        */ {1.0f, 1.4f, 1.0f, 0.9f, 1.0f, 1.6f, 1.0f, 0.7f, 0.9f, 1.3f, 1.4f, 1.0f, 1.0f},
        /* TrainProfession    */ {1.0f, 0.9f, 1.0f, 0.9f, 1.0f, 0.9f, 1.0f, 0.7f, 0.6f, 1.1f, 1.0f, 3.0f, 2.5f},
        /* RestockConsumables */ {1.0f, 1.0f, 1.0f, 0.9f, 1.0f, 1.0f, 1.0f, 0.9f, 0.8f, 2.2f, 1.2f, 1.0f, 1.0f},
        /* ClearInventory     */ {1.0f, 0.5f, 0.8f, 0.7f, 0.9f, 0.7f, 0.9f, 0.8f, 0.5f, 3.0f, 2.0f, 0.3f, 1.0f},
        /* Socialise          */ {1.0f, 0.8f, 1.6f, 1.2f, 2.0f, 1.0f, 1.2f, 1.5f, 1.0f, 1.0f, 1.0f, 0.8f, 1.0f},
    };

    constexpr uint8 ARCHETYPE_COUNT = static_cast<uint8>(BotArchetype::Max);

    /**
     * Per-archetype baseline, as a multiplier on the configured global weights.
     *
     * These are what make the realm look inhabited rather than simulated. A Socialite hanging around
     * a city and a Gatherer working the hills are running the same code with different numbers here.
     */
    float const ARCHETYPE_BASE[ARCHETYPE_COUNT][RPG_STATUS_END] = {
        // IDLE GRIND CAMP WANDR NPC  QUEST FLIGHT REST PVP  VENDOR MAIL GATHER TRAIN
        /* Questor   */ {1.0f, 1.0f, 0.8f, 0.9f, 1.1f, 2.2f, 1.4f, 0.9f, 0.7f, 1.0f, 1.0f, 0.7f, 1.0f},
        /* Gatherer  */ {1.0f, 0.8f, 0.8f, 1.0f, 0.8f, 0.7f, 1.1f, 0.9f, 0.5f, 1.3f, 1.2f, 2.6f, 1.3f},
        /* Grinder   */ {1.0f, 2.4f, 0.7f, 1.4f, 0.7f, 0.8f, 0.9f, 1.0f, 1.0f, 1.0f, 0.9f, 0.8f, 0.9f},
        /* Trader    */ {1.0f, 0.6f, 1.4f, 0.8f, 1.2f, 0.7f, 1.2f, 1.0f, 0.5f, 1.8f, 2.0f, 1.1f, 1.2f},
        /* Socialite */ {1.0f, 0.5f, 2.2f, 1.3f, 2.4f, 0.8f, 1.1f, 1.6f, 0.6f, 1.1f, 1.0f, 0.6f, 0.9f},
        /* PvPer     */ {1.0f, 1.3f, 0.9f, 1.1f, 0.8f, 0.8f, 1.2f, 0.9f, 3.0f, 1.0f, 0.9f, 0.6f, 0.9f},
    };

    char const* ArchetypeName(BotArchetype type)
    {
        switch (type)
        {
            case BotArchetype::Questor:   return "Questor";
            case BotArchetype::Gatherer:  return "Gatherer";
            case BotArchetype::Grinder:   return "Grinder";
            case BotArchetype::Trader:    return "Trader";
            case BotArchetype::Socialite: return "Socialite";
            case BotArchetype::PvPer:     return "PvPer";
            default:                      return "?";
        }
    }

    char const* GoalName(BotGoalType type)
    {
        switch (type)
        {
            case BotGoalType::LevelUp:            return "LevelUp";
            case BotGoalType::EarnGold:           return "EarnGold";
            case BotGoalType::AcquireGear:        return "AcquireGear";
            case BotGoalType::TrainProfession:    return "TrainProfession";
            case BotGoalType::RestockConsumables: return "RestockConsumables";
            case BotGoalType::ClearInventory:     return "ClearInventory";
            case BotGoalType::Socialise:          return "Socialise";
            default:                              return "?";
        }
    }
}  // namespace

void BotAgendaMgr::Load()
{
    uint32 const oldMSTime = getMSTime();

    std::unordered_map<ObjectGuid, Agenda> loaded;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT guid, type, param, target, progress, created_at, expires_at FROM playerbot_goal"))
    {
        do
        {
            Field* fields = result->Fetch();

            BotGoal goal;
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
            goal.type = static_cast<BotGoalType>(fields[1].Get<uint8>());
            goal.param = fields[2].Get<uint32>();
            goal.target = fields[3].Get<int64>();
            goal.progress = fields[4].Get<int64>();
            goal.createdAt = fields[5].Get<uint32>();
            goal.expiresAt = fields[6].Get<uint32>();

            if (goal.type >= BotGoalType::Max)
                continue;

            loaded[guid].push_back(goal);
        } while (result->NextRow());
    }

    std::unordered_map<ObjectGuid, BotArchetype> profiles;
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT guid, archetype FROM playerbot_profile"))
    {
        do
        {
            Field* fields = result->Fetch();
            BotArchetype const archetype = static_cast<BotArchetype>(fields[1].Get<uint8>());
            if (archetype < BotArchetype::Max)
                profiles[ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>())] = archetype;
        } while (result->NextRow());
    }

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _agendas = std::move(loaded);
        _archetypes = std::move(profiles);
    }

    LOG_INFO("server.loading", ">> Loaded {} playerbot archetype profiles", _archetypes.size());

    LOG_INFO("server.loading", ">> Loaded agendas for {} playerbots in {} ms", _agendas.size(),
             GetMSTimeDiffToNow(oldMSTime));
}

BotArchetype BotAgendaMgr::RollArchetype()
{
    uint32 const shares[ARCHETYPE_COUNT] = {
        sPlayerbotAIConfig.archetypeShareQuestor,   sPlayerbotAIConfig.archetypeShareGatherer,
        sPlayerbotAIConfig.archetypeShareGrinder,   sPlayerbotAIConfig.archetypeShareTrader,
        sPlayerbotAIConfig.archetypeShareSocialite, sPlayerbotAIConfig.archetypeSharePvPer};

    uint32 total = 0;
    for (uint32 share : shares)
        total += share;

    // An operator who zeroes every share gets the old uniform behaviour rather than a division by
    // zero, which is the sane reading of "I do not want archetypes".
    if (!total)
        return BotArchetype::Questor;

    uint32 roll = urand(1, total);
    for (uint8 i = 0; i < ARCHETYPE_COUNT; ++i)
    {
        if (roll <= shares[i])
            return static_cast<BotArchetype>(i);
        roll -= shares[i];
    }

    return BotArchetype::Questor;
}

BotArchetype BotAgendaMgr::GetArchetype(Player* bot)
{
    if (!bot)
        return BotArchetype::Questor;

    ObjectGuid const guid = bot->GetGUID();

    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        auto itr = _archetypes.find(guid);
        if (itr != _archetypes.end())
            return itr->second;
    }

    BotArchetype const rolled = RollArchetype();

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        // Another thread may have assigned one while the shared lock was released.
        auto const [itr, inserted] = _archetypes.emplace(guid, rolled);
        if (!inserted)
            return itr->second;
    }

    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbot_profile (guid, archetype, assigned_at) VALUES ({}, {}, {}) "
        "ON DUPLICATE KEY UPDATE archetype = archetype",
        guid.GetCounter(), static_cast<uint32>(rolled),
        static_cast<uint32>(GameTime::GetGameTime().count()));

    return rolled;
}

std::string BotAgendaMgr::DescribeDistribution() const
{
    uint32 counts[ARCHETYPE_COUNT] = {};
    uint32 total = 0;

    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        for (auto const& [guid, archetype] : _archetypes)
        {
            if (archetype < BotArchetype::Max)
            {
                ++counts[static_cast<uint8>(archetype)];
                ++total;
            }
        }
    }

    std::string out = Acore::StringFormat("Archetypes across {} known bots:\n", total);
    for (uint8 i = 0; i < ARCHETYPE_COUNT; ++i)
        out += Acore::StringFormat("  {:<10} {:>5}  ({:.1f}%)\n", ArchetypeName(static_cast<BotArchetype>(i)),
                                   counts[i], total ? 100.0f * counts[i] / total : 0.0f);

    return out;
}

void BotAgendaMgr::Forget(ObjectGuid guid)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _agendas.erase(guid);
}

void BotAgendaMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.agendaEnabled)
        return;

    _timer += diff;
    if (_timer < sPlayerbotAIConfig.agendaTickMs)
        return;

    _timer = 0;

    // Round-robin with a fixed budget: cost per tick is independent of realm size. A goal changes
    // over minutes, so revisiting a given bot every few seconds rather than every tick loses
    // nothing and keeps this off the profile at three thousand bots.
    std::vector<Player*> bots = sRandomPlayerbotMgr.GetPlayers();
    if (bots.empty())
        return;

    uint32 const budget = std::min<uint32>(sPlayerbotAIConfig.agendaBotsPerTick, bots.size());
    for (uint32 i = 0; i < budget; ++i)
    {
        Player* bot = bots[(_cursor + i) % bots.size()];
        if (bot && bot->IsInWorld())
            Evaluate(bot);
    }

    _cursor = (_cursor + budget) % bots.size();
}

void BotAgendaMgr::Evaluate(Player* bot)
{
    uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

    // Assign on first sight. Doing it here rather than at character creation means bots that predate
    // the feature acquire one on their next tick instead of needing a migration.
    GetArchetype(bot);

    Agenda agenda;
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        auto itr = _agendas.find(bot->GetGUID());
        if (itr != _agendas.end())
            agenda = itr->second;
    }

    // Expired goals go before anything else looks at them.
    agenda.erase(std::remove_if(agenda.begin(), agenda.end(),
                                [now](BotGoal const& goal)
                                { return goal.expiresAt && goal.expiresAt <= now; }),
                 agenda.end());

    // Derived goals are rebuilt from the character every tick rather than stored, so they can never
    // disagree with it.
    agenda.erase(std::remove_if(agenda.begin(), agenda.end(),
                                [](BotGoal const& goal) { return goal.IsDerived(); }),
                 agenda.end());

    // ClearInventory is always present and its priority is a function of free space. When bags are
    // nearly full it outranks everything, which is what keeps the economic loop from deadlocking:
    // a bot that cannot loot cannot gather, quest, or supply the auction house.
    {
        uint32 const free = bot->GetFreeInventorySpace();
        BotGoal goal;
        goal.type = BotGoalType::ClearInventory;
        goal.target = sPlayerbotAIConfig.agendaFreeSlotTarget;
        goal.progress = free;
        goal.createdAt = now;
        goal.priority = free >= sPlayerbotAIConfig.agendaFreeSlotTarget
                            ? 0
                            : static_cast<uint8>(std::min<uint32>(
                                  255, 255 * (sPlayerbotAIConfig.agendaFreeSlotTarget - free) /
                                           std::max<uint32>(1, sPlayerbotAIConfig.agendaFreeSlotTarget)));
        agenda.push_back(goal);
    }

    if (bot->GetLevel() < sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
    {
        BotGoal goal;
        goal.type = BotGoalType::LevelUp;
        goal.target = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        goal.progress = bot->GetLevel();
        goal.createdAt = now;
        // Strongest early, easing off as the bot approaches the cap, so low-level bots quest hard
        // and near-max bots start doing other things with their time.
        goal.priority = static_cast<uint8>(
            std::clamp<int32>(220 - (bot->GetLevel() * 160 / std::max<uint32>(1, goal.target)), 40, 220));
        agenda.push_back(goal);
    }

    // Generate the goals a bot ought to have but does not yet. Doing this here rather than at bot
    // creation means a bot that picks up a profession later, or spends its savings, acquires the
    // matching goal on its next tick instead of carrying whatever it was given at birth.
    bool dirty = false;

    auto const hasGoal = [&agenda](BotGoalType type, uint32 param)
    {
        return std::any_of(agenda.begin(), agenda.end(), [type, param](BotGoal const& goal)
                           { return goal.type == type && goal.param == param; });
    };

    for (uint32 skill : {SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
                         SKILL_HERBALISM, SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING,
                         SKILL_MINING, SKILL_SKINNING, SKILL_TAILORING})
    {
        if (!bot->HasSkill(skill) || hasGoal(BotGoalType::TrainProfession, skill))
            continue;

        uint32 const cap = bot->GetPureMaxSkillValue(skill);
        if (!cap || bot->GetSkillValue(skill) >= cap)
            continue;

        BotGoal goal;
        goal.type = BotGoalType::TrainProfession;
        goal.param = skill;
        goal.target = cap;
        goal.progress = bot->GetSkillValue(skill);
        goal.createdAt = now;
        agenda.push_back(goal);
        dirty = true;
    }

    // Something to save for. Roughly the cost of the next thing a player at this level wants:
    // a mount, then a faster one, then flying.
    if (!hasGoal(BotGoalType::EarnGold, 0))
    {
        uint32 const level = bot->GetLevel();
        int64 const target = level < 20 ? 10 * GOLD : level < 40 ? 100 * GOLD
                                                    : level < 60 ? 600 * GOLD
                                                                 : 1000 * GOLD;

        BotGoal goal;
        goal.type = BotGoalType::EarnGold;
        goal.target = target;
        goal.progress = bot->GetMoney();
        goal.createdAt = now;
        agenda.push_back(goal);
        dirty = true;
    }

    for (BotGoal& goal : agenda)
    {
        if (goal.IsDerived())
            continue;

        switch (goal.type)
        {
            case BotGoalType::EarnGold:
                goal.progress = bot->GetMoney();
                goal.priority = goal.progress >= goal.target
                                    ? 0
                                    : static_cast<uint8>(std::min<int64>(
                                          255, 255 - (goal.progress * 255 / std::max<int64>(1, goal.target))));
                break;

            case BotGoalType::TrainProfession:
                goal.progress = bot->GetSkillValue(goal.param);
                goal.priority = goal.progress >= goal.target
                                    ? 0
                                    : static_cast<uint8>(std::min<int64>(
                                          200, 200 - (goal.progress * 200 / std::max<int64>(1, goal.target))));
                break;

            default:
                // Not yet generated by anything; kept so stored rows survive a round trip.
                break;
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _agendas[bot->GetGUID()] = agenda;
    }

    // Only when the persistent set actually changed. Priorities and progress move every tick and
    // are cheap to recompute on load, so writing them back each time would be a lot of database
    // traffic to preserve numbers that are stale the moment they land.
    if (dirty)
        Save(bot->GetGUID(), agenda);
}

float BotAgendaMgr::GetActivityMultiplier(Player* bot, NewRpgStatus status) const
{
    if (!bot || !sPlayerbotAIConfig.agendaEnabled || status < 0 || status >= RPG_STATUS_END)
        return 1.0f;

    std::shared_lock<std::shared_mutex> lock(_mutex);

    // Archetype sets the baseline; goals push around it. Read without assigning, because this is a
    // const query on a hot path -- assignment happens during Evaluate.
    float archetypeBase = 1.0f;
    auto profile = _archetypes.find(bot->GetGUID());
    if (profile != _archetypes.end() && profile->second < BotArchetype::Max)
        archetypeBase = ARCHETYPE_BASE[static_cast<uint8>(profile->second)][status];

    auto itr = _agendas.find(bot->GetGUID());
    if (itr == _agendas.end())
        return archetypeBase;

    float multiplier = 1.0f;
    for (BotGoal const& goal : itr->second)
    {
        if (!goal.priority || goal.type >= BotGoalType::Max)
            continue;

        float const affinity = AFFINITY[static_cast<uint8>(goal.type)][status];
        multiplier += (affinity - 1.0f) * (goal.priority / 255.0f);
    }

    // A goal may discourage an activity but never forbid it: availability is CheckRpgStatusAvailable's
    // job, and a bot that can literally never choose an activity is a bot with a hidden deadlock.
    return std::clamp(archetypeBase * multiplier, 0.1f, 8.0f);
}

std::string BotAgendaMgr::DescribeAgenda(Player* bot) const
{
    if (!bot)
        return "no bot";

    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto itr = _agendas.find(bot->GetGUID());
    if (itr == _agendas.end() || itr->second.empty())
        return Acore::StringFormat("{} has no agenda yet", bot->GetName());

    std::string archetype = "unassigned";
    if (auto profile = _archetypes.find(bot->GetGUID()); profile != _archetypes.end())
        archetype = ArchetypeName(profile->second);

    std::string out = Acore::StringFormat("{} [{}] agenda:\n", bot->GetName(), archetype);
    for (BotGoal const& goal : itr->second)
        out += Acore::StringFormat("  {:<19} priority {:>3}  progress {} / {}\n", GoalName(goal.type),
                                   goal.priority, goal.progress, goal.target);

    return out;
}

void BotAgendaMgr::Save(ObjectGuid guid, Agenda const& agenda)
{
    PlayerbotsDatabase.Execute("DELETE FROM playerbot_goal WHERE guid = {}", guid.GetCounter());

    for (BotGoal const& goal : agenda)
    {
        if (goal.IsDerived())
            continue;

        PlayerbotsDatabase.Execute(
            "INSERT INTO playerbot_goal (guid, type, param, target, progress, created_at, expires_at) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE target = {}, progress = {}, expires_at = {}",
            guid.GetCounter(), static_cast<uint32>(goal.type), goal.param, goal.target, goal.progress,
            goal.createdAt, goal.expiresAt, goal.target, goal.progress, goal.expiresAt);
    }
}
