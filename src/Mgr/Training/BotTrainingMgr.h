/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTTRAININGMGR_H
#define PLAYERBOTS_BOTTRAININGMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

/**
 * Keeps bots' spells current, on a timer, independent of the strategy engine.
 *
 * Training used to hang off a `seldom` trigger inside the loot strategy, competing at relevance 1.0
 * with vendoring, auctioning and disenchanting. For most bots that was enough. For a self bot it was
 * not: one stood beside its own trainer with money in its pocket and never trained, and the action
 * was never even evaluated -- no gate rejected it, the engine simply never got to it.
 *
 * A behaviour that must always happen should not be arbitrated by a queue that may never reach it.
 * This runs from the per-bot update hook, like the safety and loot-roll managers, so it happens for
 * random bots, alt bots and self bots alike, and does not depend on which strategies a bot carries.
 *
 * Spending order within a pass is highest requirement first, cheapest within a rank, so a bot buys
 * the rank it has just become eligible for rather than whatever the trainer table happened to list
 * first.
 */
class BotTrainingMgr
{
public:
    static BotTrainingMgr& instance()
    {
        static BotTrainingMgr instance;
        return instance;
    }

    /// Per-bot tick. Trains on its own interval; cheap on every other call.
    void Update(Player* bot, uint32 diff);

    /**
     * Strip any primary profession this bot's class should not have.
     *
     * A last line rather than a first one. Something in the login path grants professions without
     * an apprentice spell -- 190 such skills appeared across all eleven professions the moment two
     * hundred bots logged in -- and I could not find it by reading. Predicting the source has now
     * failed three times, so this stops predicting: it restates the invariant every pass, and is
     * correct whatever put the skill there.
     */
    void EnforceProfessions(Player* bot);

    /// Remove spells whose skill line restricts them to a class this bot is not.
    void EnforceClassSpells(Player* bot);

    /// Train immediately, ignoring the interval. Returns how many spells were learned.
    uint32 TrainNow(Player* bot);

    /// Total cost of everything this bot could train right now, for the spending budget.
    uint32 PendingCost(Player* bot);

    /// Forget a bot's timer, e.g. on logout.
    void Forget(ObjectGuid guid);

    std::string DescribeStats() const;

private:
    BotTrainingMgr() = default;
    ~BotTrainingMgr() = default;

    BotTrainingMgr(BotTrainingMgr const&) = delete;
    BotTrainingMgr& operator=(BotTrainingMgr const&) = delete;

    struct TrainableSpell
    {
        uint32 spellId{0};
        uint32 cost{0};
        uint32 reqSkillLine{0};
        uint32 reqSkillRank{0};
        uint32 reqLevel{0};
        uint32 reqSpell{0};
    };

    /// Load once from npc_trainer, the same table a real trainer reads.
    void EnsureLoaded();

    /// True if this bot may spend its own gold on training.
    static bool MaySpend(Player* bot);

    /// The spell a trainer entry actually teaches, unwrapping a "learn spell" effect.
    static uint32 TaughtSpell(uint32 spellId);

    /// The primary profession skill a spell teaches, or 0 if it teaches something else.
    static uint32 PrimaryProfessionTaught(uint32 spellId);

    /// The two primary professions this bot should end up with, chosen stably from its guid.
    static std::pair<uint16, uint16> PreferredProfessions(Player* bot);

    /// True if this bot meets every requirement a real trainer would check for this spell.
    static bool Qualifies(Player* bot, TrainableSpell const& entry);

    std::once_flag _loadOnce;
    std::vector<TrainableSpell> _spells;

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, uint32> _timers;

    /// Spells a bot paid for and did not receive. Never attempted again for that bot.
    std::unordered_map<ObjectGuid, std::unordered_set<uint32>> _unteachable;

    /// Spells each bot has already bought once, so a second attempt can be recognised.
    std::unordered_map<ObjectGuid, std::unordered_set<uint32>> _purchased;

    /**
     * Spells that did not survive a relog for some bot, and so will not for any.
     *
     * Some trainer entries teach a proficiency rather than a spellbook entry: HasSpell answers yes
     * immediately after learning, so the purchase verifies, but nothing is written to
     * character_spell and the next login finds the bot eligible again. Realm-wide, spell 2581 and
     * 7414 are known by zero characters while a real spell like Judgement is known by 49.
     *
     * Per-bot detection would make every bot pay to discover this separately. One bot discovering
     * it is enough for all of them.
     */
    std::unordered_set<uint32> _neverPersists;

    uint32 _refunded{0};
    uint32 _professionRefused{0};
    uint32 _professionStripped{0};
    uint32 _learnedTotal{0};
    uint32 _passesRun{0};
};

#define sBotTrainingMgr BotTrainingMgr::instance()

#endif
