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

            TrainableSpell entry;
            entry.spellId = fields[0].Get<uint32>();
            entry.cost = fields[1].Get<uint32>();
            entry.reqSkillLine = fields[2].Get<uint32>();
            entry.reqSkillRank = fields[3].Get<uint32>();
            entry.reqLevel = fields[4].Get<uint32>();
            entry.reqSpell = fields[5].Get<uint32>();

            if (entry.spellId)
                g_trainable.push_back(entry);
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", ">> Loaded {} trainable spells for remote training", g_trainable.size());
}
}  // namespace

bool RemoteTrainAction::isUseful()
{
    if (!sPlayerbotAIConfig.remoteTrainingEnabled)
        return false;

    // A bot under a human's command does not spend its owner's gold unasked.
    if (botAI->GetMaster() && !GET_PLAYERBOT_AI(botAI->GetMaster()))
        return false;

    return bot->GetMoney() > 0;
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

        if (spent + entry.cost > budget)
            continue;

        bot->ModifyMoney(-static_cast<int32>(entry.cost));
        bot->learnSpell(entry.spellId);

        spent += entry.cost;
        ++learned;

        LOG_DEBUG("playerbots", "[Train] {} learned spell {} for {}c without visiting a trainer", bot->GetName(),
                  entry.spellId, entry.cost);
    }

    return learned > 0;
}
