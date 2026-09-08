/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RemoteTrainAction.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QueryResult.h"
#include "SpellMgr.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
struct TrainableSpell
{
    uint32 spellId;
    uint32 cost;
    uint32 reqSkillLine;
    uint32 reqSkillRank;
    uint32 reqLevel;
    uint32 reqSpell;
};

std::vector<TrainableSpell> g_trainable;
std::once_flag g_loaded;

/// Loaded once from npc_trainer, which is the same table a real trainer reads.
void LoadTrainableSpells()
{
    if (QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT SpellID, MoneyCost, ReqSkillLine, ReqSkillRank, ReqLevel, ReqSpell FROM npc_trainer"))
    {
        do
        {
            Field* fields = result->Fetch();

            // npc_trainer uses a NEGATIVE SpellID to mean "include trainer template N", not a spell.
            // Read as uint32 those became huge values that passed the `if (entry.spellId)` check and
            // padded the list with thousands of entries that can never match anything.
            int32 const spellId = fields[0].Get<int32>();
            if (spellId <= 0)
                continue;

            TrainableSpell entry;
            entry.spellId = uint32(spellId);
            entry.cost = fields[1].Get<uint32>();
            entry.reqSkillLine = fields[2].Get<uint32>();
            entry.reqSkillRank = fields[3].Get<uint32>();
            entry.reqLevel = fields[4].Get<uint32>();
            entry.reqSpell = fields[5].Get<uint32>();

            g_trainable.push_back(entry);
        } while (result->NextRow());
    }

    // Highest requirement first, cheapest first within a rank.
    //
    // The list was previously walked in whatever order npc_trainer returned, and only the first few
    // affordable spells per pass were bought. A bot could therefore spend its whole allowance on
    // low-rank filler and never reach the rank it had just become eligible for -- which is exactly
    // what "my paladin still has not bought Judgement" looks like from the outside.
    std::sort(g_trainable.begin(), g_trainable.end(), [](TrainableSpell const& a, TrainableSpell const& b)
    {
        if (a.reqLevel != b.reqLevel)
            return a.reqLevel > b.reqLevel;
        return a.cost < b.cost;
    });

    LOG_INFO("server.loading", ">> Loaded {} trainable spells for remote training", g_trainable.size());
}
}  // namespace

bool RemoteTrainAction::isUseful()
{
    if (!sPlayerbotAIConfig.remoteTrainingEnabled)
        return false;

    // A bot under a human's command does not spend its owner's gold unasked. A self bot is its own
    // master, so it passes this and trains for itself; an alt bot following a real player does not.
    if (botAI->GetMaster() && !GET_PLAYERBOT_AI(botAI->GetMaster()))
    {
        LOG_DEBUG("playerbots", "[Train] {} skipped: under a human master's command", bot->GetName());
        return false;
    }

    if (!bot->GetMoney())
    {
        LOG_DEBUG("playerbots", "[Train] {} skipped: no money", bot->GetName());
        return false;
    }

    return true;
}

bool RemoteTrainAction::Execute(Event /*event*/)
{
    std::call_once(g_loaded, LoadTrainableSpells);

    uint32 learned = 0;
    uint32 const budget = bot->GetMoney();
    uint32 spent = 0;

    for (TrainableSpell const& entry : g_trainable)
    {
        if (learned >= sPlayerbotAIConfig.remoteTrainingMaxPerPass)
            break;

        if (bot->HasSpell(entry.spellId))
            continue;

        // Every gate a real trainer would apply.
        if (entry.reqLevel && bot->GetLevel() < entry.reqLevel)
            continue;

        if (entry.reqSpell && !bot->HasSpell(entry.reqSpell))
            continue;

        if (entry.reqSkillLine && bot->GetBaseSkillValue(entry.reqSkillLine) < entry.reqSkillRank)
            continue;

        // Only what this class or its professions can actually use. Without this a bot would learn
        // every trainable spell in the game, which is both nonsense and a way to make one bot
        // capable of everything.
        if (!bot->IsSpellFitByClassAndRace(entry.spellId))
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(entry.spellId);
        if (!spellInfo)
            continue;

        // continue, not break: the list is ordered by rank, so a spell that is out of reach is
        // followed by cheaper lower ranks that are not.
        if (spent + entry.cost > budget)
            continue;

        bot->ModifyMoney(-static_cast<int32>(entry.cost));
        bot->learnSpell(entry.spellId);

        spent += entry.cost;
        ++learned;

        LOG_DEBUG("playerbots", "[Train] {} learned spell {} (rank req {}) for {}c without visiting a trainer",
                  bot->GetName(), entry.spellId, entry.reqLevel, entry.cost);
    }

    // Silence here is the case that has been hard to diagnose from the outside: it looks identical
    // to the action never running at all. Say which it was.
    if (!learned)
        LOG_DEBUG("playerbots", "[Train] {} (level {}, {}c) ran and learned nothing from {} candidates",
                  bot->GetName(), bot->GetLevel(), budget, uint32(g_trainable.size()));

    return learned > 0;
}
