/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTrainingMgr.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QueryResult.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>

namespace
{
/// How often a bot considers training. Frequent enough that a new rank is bought within a minute of
/// becoming available, rare enough that the query-free pass costs nothing at 500 bots.
constexpr uint32 TRAIN_INTERVAL_MS = 30 * 1000;
}  // namespace

void BotTrainingMgr::EnsureLoaded()
{
    std::call_once(_loadOnce, [this]()
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT SpellID, MoneyCost, ReqSkillLine, ReqSkillRank, ReqLevel, ReqSpell FROM npc_trainer");

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();

                // A negative SpellID means "include trainer template N", not a spell.
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

                _spells.push_back(entry);
            } while (result->NextRow());
        }

        // Highest requirement first, cheapest within a rank: a bot should buy the rank it has just
        // become eligible for, not whatever the table happened to list first.
        std::sort(_spells.begin(), _spells.end(), [](TrainableSpell const& a, TrainableSpell const& b)
        {
            if (a.reqLevel != b.reqLevel)
                return a.reqLevel > b.reqLevel;
            return a.cost < b.cost;
        });

        LOG_INFO("server.loading", ">> Loaded {} trainable spells for bot training", _spells.size());
    });
}

bool BotTrainingMgr::MaySpend(Player* bot)
{
    // Any bot spends its own gold on its own spells.
    //
    // This used to refuse alt bots on the grounds that a bot under a human's command should not
    // spend its owner's gold unasked. That reasoning does not survive contact with what an alt bot
    // is: the gold is in the alt's own pocket, and an alt that will not train is an alt that stays
    // useless until its owner drives it to a trainer by hand -- which is the chore the AI exists to
    // remove. The owner decides how much gold the alt carries; that is the real control.
    return GET_PLAYERBOT_AI(bot) != nullptr;
}

bool BotTrainingMgr::Qualifies(Player* bot, TrainableSpell const& entry)
{
    if (bot->HasSpell(entry.spellId))
        return false;

    if (entry.reqLevel && bot->GetLevel() < entry.reqLevel)
        return false;

    if (entry.reqSpell && !bot->HasSpell(entry.reqSpell))
        return false;

    if (entry.reqSkillLine && bot->GetBaseSkillValue(entry.reqSkillLine) < entry.reqSkillRank)
        return false;

    // Only what this class or race can actually use. Without this a bot would learn every trainable
    // spell in the game, which is both nonsense and a way to make one bot capable of everything.
    if (!bot->IsSpellFitByClassAndRace(entry.spellId))
        return false;

    return sSpellMgr->GetSpellInfo(entry.spellId) != nullptr;
}

uint32 BotTrainingMgr::TrainNow(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !sPlayerbotAIConfig.remoteTrainingEnabled)
        return 0;

    if (!MaySpend(bot))
        return 0;

    EnsureLoaded();

    uint32 const budget = bot->GetMoney();
    if (!budget)
        return 0;

    uint32 learned = 0;
    uint32 spent = 0;

    for (TrainableSpell const& entry : _spells)
    {
        if (learned >= sPlayerbotAIConfig.remoteTrainingMaxPerPass)
            break;

        if (!Qualifies(bot, entry))
            continue;

        // continue, not break: the list runs highest rank first, so an entry that is out of reach is
        // followed by cheaper lower ranks that are not.
        if (spent + entry.cost > budget)
            continue;

        bot->ModifyMoney(-int32(entry.cost));
        bot->learnSpell(entry.spellId);

        spent += entry.cost;
        ++learned;

        LOG_DEBUG("playerbots", "[Train] {} learned spell {} (rank req {}) for {}c", bot->GetName(), entry.spellId,
                  entry.reqLevel, entry.cost);
    }

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _learnedTotal += learned;
        ++_passesRun;
    }

    return learned;
}

uint32 BotTrainingMgr::PendingCost(Player* bot)
{
    if (!bot || !sPlayerbotAIConfig.remoteTrainingEnabled)
        return 0;

    EnsureLoaded();

    uint32 total = 0;
    for (TrainableSpell const& entry : _spells)
        if (Qualifies(bot, entry))
            total += entry.cost;

    return total;
}

void BotTrainingMgr::Update(Player* bot, uint32 diff)
{
    if (!bot || !sPlayerbotAIConfig.remoteTrainingEnabled)
        return;

    ObjectGuid const guid = bot->GetGUID();

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        uint32& timer = _timers[guid];

        if (timer > diff)
        {
            timer -= diff;
            return;
        }

        timer = TRAIN_INTERVAL_MS;
    }

    TrainNow(bot);
}

void BotTrainingMgr::Forget(ObjectGuid guid)
{
    std::unique_lock<std::shared_mutex> guard(_mutex);
    _timers.erase(guid);
}

std::string BotTrainingMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("training: {} spells learned over {} passes, {} bots tracked", _learnedTotal, _passesRun,
                               _timers.size());
}
