/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LOOTNONCOMBATSTRATEGY_H
#define PLAYERBOTS_LOOTNONCOMBATSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

class LootNonCombatStrategy : public Strategy
{
public:
    LootNonCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "loot"; }
};

/// Operator-selectable bias toward gathering (R5.1).
///
/// Does not add new mechanics - it raises the relevance of the existing `new rpg gather` action so
/// a bot running it prefers walking node routes over the other RPG activities. Enable with
/// `+farm materials` on a bot or via AiPlayerbot.RandomBotNonCombatStrategies.
class FarmMaterialsStrategy : public Strategy
{
public:
    FarmMaterialsStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "farm materials"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class GatherStrategy : public Strategy
{
public:
    GatherStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "gather"; }
};

class RevealStrategy : public Strategy
{
public:
    RevealStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "reveal"; }
};

class UseBobberStrategy : public Strategy
{
public:
    UseBobberStrategy(PlayerbotAI* botAI) : Strategy(botAI){}
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override {return "use bobber";}
};

#endif
