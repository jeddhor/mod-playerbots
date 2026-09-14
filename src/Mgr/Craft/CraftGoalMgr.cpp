/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftGoalMgr.h"

#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "ReagentSourceMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>

namespace
{
/// Enough failures realm-wide that the recipe has proved it cannot be worked, whatever the index says.
constexpr uint32 FAILURES_BEFORE_BLACKLIST = 5;

/// The item a crafting spell produces, or 0.
uint32 CreatedItemOf(SpellInfo const* info)
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (info->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[i].ItemType)
            return info->Effects[i].ItemType;

    return 0;
}
}  // namespace

void CraftGoalMgr::Load()
{
    std::unique_lock<std::shared_mutex> guard(_mutex);

    uint32 const now = uint32(GameTime::GetGameTime().count());

    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT guid, spell, item, created_at, expires_at FROM playerbot_craft_goal"))
        do
        {
            Field* fields = result->Fetch();
            Goal goal;
            goal.spellId = fields[1].Get<uint32>();
            goal.itemId = fields[2].Get<uint32>();
            goal.createdAt = fields[3].Get<uint32>();
            goal.expiresAt = fields[4].Get<uint32>();

            // Expired while the realm was down. Dropped rather than loaded, so a restart does not
            // resurrect a goal that had already run out of time.
            if (goal.expiresAt && goal.expiresAt <= now)
                continue;

            _goals[fields[0].Get<uint32>()] = goal;
        } while (result->NextRow());

    if (QueryResult result = PlayerbotsDatabase.Query("SELECT spell, failures FROM playerbot_craft_goal_failure"))
        do
        {
            Field* fields = result->Fetch();
            _failures[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (result->NextRow());

    LOG_INFO("playerbots", "[CraftGoal] loaded {} live goals and {} failing recipes",
             uint32(_goals.size()), uint32(_failures.size()));
}

void CraftGoalMgr::Store(ObjectGuid::LowType guid, Goal const& goal)
{
    PlayerbotsDatabase.Execute(
        "REPLACE INTO playerbot_craft_goal (guid, spell, item, created_at, expires_at) VALUES ({}, {}, {}, {}, {})",
        guid, goal.spellId, goal.itemId, goal.createdAt, goal.expiresAt);
}

void CraftGoalMgr::Forget(ObjectGuid::LowType guid)
{
    PlayerbotsDatabase.Execute("DELETE FROM playerbot_craft_goal WHERE guid = {}", guid);
}

std::optional<CraftGoalMgr::Goal> CraftGoalMgr::Current(Player* bot)
{
    if (!bot)
        return std::nullopt;

    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    uint32 const now = uint32(GameTime::GetGameTime().count());

    Goal goal;
    {
        std::shared_lock<std::shared_mutex> guard(_mutex);
        auto const itr = _goals.find(guid);
        if (itr == _goals.end())
            return std::nullopt;

        goal = itr->second;
    }

    if (!goal.expiresAt || goal.expiresAt > now)
        return goal;

    // Expired after the bot had actually worked it: a failure, since the bot had its window and did
    // not finish -- the signal that this recipe is not workable in practice however good the index
    // looked. One that expired unworked only means the bot was busy, and is dropped without blame.
    if (goal.worked)
        Abandon(bot, goal, "expired");
    else
    {
        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            _goals.erase(guid);
        }
        Forget(guid);
    }

    return std::nullopt;
}

void CraftGoalMgr::MarkWorked(Player* bot)
{
    if (!bot)
        return;

    std::unique_lock<std::shared_mutex> guard(_mutex);
    auto const itr = _goals.find(bot->GetGUID().GetCounter());
    if (itr != _goals.end())
        itr->second.worked = true;
}

std::optional<CraftGoalMgr::Goal> CraftGoalMgr::Choose(Player* bot, bool commit)
{
    if (!bot || !sPlayerbotAIConfig.craftGoalEnabled)
        return std::nullopt;

    if (std::optional<Goal> existing = Current(bot))
        return existing;

    uint32 bestSpell = 0;
    uint32 bestItem = 0;
    uint32 bestGreyAt = 0;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        {
            std::shared_lock<std::shared_mutex> guard(_mutex);
            auto const failed = _failures.find(spellId);
            if (failed != _failures.end() && failed->second >= FAILURES_BEFORE_BLACKLIST)
                continue;
        }

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        uint32 const itemId = CreatedItemOf(info);
        if (!itemId)
            continue;

        // Still worth skill. A craft that teaches nothing trades materials for something the bot
        // could usually have bought, which is not worth a cross-zone errand.
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineAbilityEntry const* ability = itr->second;
            if (!ability || !ability->SkillLine || !ability->TrivialSkillLineRankHigh)
                continue;

            uint16 const skill = bot->GetPureSkillValue(ability->SkillLine);
            if (!skill || skill < ability->MinSkillLineRank || skill >= ability->TrivialSkillLineRankHigh)
                continue;

            if (bestSpell && ability->TrivialSkillLineRankHigh >= bestGreyAt)
                continue;

            // Only if something is actually missing -- otherwise BotCraftMgr's own pass will make it
            // this tick and a goal would be pure overhead -- and only if what is missing can be
            // farmed. Buying is Phase 5's job; a goal that cannot be worked is a slot wasted.
            bool anyMissing = false;
            bool allMissingFarmable = true;

            for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            {
                if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
                    continue;

                uint32 const reagent = uint32(info->Reagent[r]);
                if (bot->GetItemCount(reagent, false) >= info->ReagentCount[r])
                    continue;

                anyMissing = true;
                if (!sReagentSourceMgr.BestFor(bot, reagent))
                {
                    allMissingFarmable = false;
                    break;
                }
            }

            if (!anyMissing || !allMissingFarmable)
                continue;

            bestGreyAt = ability->TrivialSkillLineRankHigh;
            bestSpell = spellId;
            bestItem = itemId;
        }
    }

    if (!bestSpell)
        return std::nullopt;

    uint32 const now = uint32(GameTime::GetGameTime().count());

    Goal goal;
    goal.spellId = bestSpell;
    goal.itemId = bestItem;
    goal.createdAt = now;
    goal.expiresAt = now + sPlayerbotAIConfig.craftGoalDurationSeconds;

    if (!commit)
        return goal;

    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _goals[guid] = goal;
        ++_chosen;
    }

    Store(guid, goal);

    LOG_DEBUG("playerbots", "[CraftGoal] {} will farm for item {} (recipe {})", bot->GetName(), bestItem, bestSpell);
    return goal;
}

CraftGoalMgr::Need CraftGoalMgr::NextNeed(Player* bot, Goal const& goal) const
{
    Need need;
    if (!bot)
        return need;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(goal.spellId);
    if (!info)
        return need;

    for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
    {
        if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
            continue;

        uint32 const reagent = uint32(info->Reagent[r]);
        uint32 const held = bot->GetItemCount(reagent, false);
        if (held >= info->ReagentCount[r])
            continue;

        need.itemId = reagent;
        need.missing = info->ReagentCount[r] - held;
        need.farmable = sReagentSourceMgr.BestFor(bot, reagent) != nullptr;
        return need;
    }

    return need;
}

void CraftGoalMgr::Complete(Player* bot, Goal const& goal)
{
    if (!bot)
        return;

    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _goals.erase(guid);
        ++_completed;

        // Success clears the recipe's history. A recipe that fails four times and then works was
        // never the problem -- the bot was -- and holding the count against it would retire a
        // perfectly good recipe on the evidence of four unlucky bots.
        _failures.erase(goal.spellId);
    }

    Forget(guid);
    PlayerbotsDatabase.Execute("DELETE FROM playerbot_craft_goal_failure WHERE spell = {}", goal.spellId);

    LOG_INFO("playerbots", "[CraftGoal] {} finished farming and crafted item {}", bot->GetName(), goal.itemId);
}

void CraftGoalMgr::Abandon(Player* bot, Goal const& goal, char const* why)
{
    if (!bot)
        return;

    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    uint32 failures = 0;

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _goals.erase(guid);
        failures = ++_failures[goal.spellId];
        ++_abandoned;
    }

    Forget(guid);
    PlayerbotsDatabase.Execute(
        "REPLACE INTO playerbot_craft_goal_failure (spell, failures) VALUES ({}, {})", goal.spellId, failures);

    LOG_DEBUG("playerbots", "[CraftGoal] {} gave up on item {} ({}); recipe {} has now failed {} times",
              bot->GetName(), goal.itemId, why, goal.spellId, failures);

    if (failures == FAILURES_BEFORE_BLACKLIST)
        LOG_INFO("playerbots", "[CraftGoal] recipe {} has failed {} times and will no longer be chosen",
                 goal.spellId, failures);
}

std::string CraftGoalMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("craft goals: {} live, {} chosen, {} completed, {} abandoned, {} recipes failing",
                               uint32(_goals.size()), _chosen, _completed, _abandoned, uint32(_failures.size()));
}
