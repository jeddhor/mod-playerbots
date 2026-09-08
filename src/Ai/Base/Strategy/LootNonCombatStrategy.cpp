/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LootNonCombatStrategy.h"
#include "Playerbots.h"

void LootNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("loot available", { NextAction("loot", 6.0f) }));
    triggers.push_back(
        new TriggerNode("far from loot target", { NextAction("move to loot", 7.0f) }));
    triggers.push_back(new TriggerNode("can loot", { NextAction("open loot", 8.0f) }));
    triggers.push_back(new TriggerNode("often", { NextAction("add all loot", 5.0f) }));
    // Low relevance on purpose: clearing junk should never win against actually looting, fighting
    // or questing. It just needs to happen eventually, before bags fill and silently disable
    // everything downstream of looting.
    triggers.push_back(new TriggerNode("often", { NextAction("vendor junk", 1.0f) }));

    // "random" (1 in 20) rather than "often": listing is bookkeeping, not gameplay, and iterating
    // every bot's bags on a fast trigger is a real cost at a few thousand bots. It was 1 in 300,
    // which produced four posts across 200 bots in forty minutes -- too little to tell a working
    // economy from a broken one.
    triggers.push_back(new TriggerNode("random", { NextAction("post auctions", 1.0f) }));
    triggers.push_back(new TriggerNode("random", { NextAction("buy auctions", 1.0f) }));

    // Enchanters break down the greens they buy and the soulbound gear they cannot sell.
    //
    // The "maintenance" strategy already carries this action but is commented out in AiFactory for
    // both random-bot paths, so no bot has ever disenchanted anything. Only this one action is
    // pulled in rather than enabling that strategy wholesale, because it also carries crafting and
    // enchanting, which have not been looked at yet and belong to the professions phase.
    //
    // This is load-bearing for the buying side: without something that consumes them, an enchanter
    // buying cheap greens just hoards them, which is worse than not buying at all.
    triggers.push_back(new TriggerNode("random", { NextAction("disenchant random item", 1.0f) }));

    // Training is what turns gathered materials into higher-tier materials, so it is the other half
    // of the economy rather than a convenience. "seldom" because a bot only needs to check
    // occasionally -- new ranks become available on level-up, not moment to moment.
    triggers.push_back(new TriggerNode("seldom", { NextAction("remote train", 1.0f) }));
}

void FarmMaterialsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Relevance 6.0 beats the 3.0 the NewRpg strategy gives every other activity, so a bot with
    // this strategy on will pick gathering whenever a workable route exists, and fall back to
    // normal RPG behaviour when one does not.
    triggers.push_back(new TriggerNode("gather status", { NextAction("new rpg gather", 6.0f) }));
}

void GatherStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("timer", { NextAction("add gathering loot", 5.0f) }));
}

void RevealStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("often", { NextAction("reveal gathering item", 50.0f) }));
}

void UseBobberStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
     triggers.push_back(
        new TriggerNode("can use fishing bobber", { NextAction("use fishing bobber", 20.0f) }));
    triggers.push_back(
        new TriggerNode("random", { NextAction("remove bobber strategy", 20.0f) }));
}
