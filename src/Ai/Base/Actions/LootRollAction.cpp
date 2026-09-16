/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotRollMgr.h"
#include "LootRollAction.h"
#include "Event.h"
#include "Group.h"
#include "ItemUsageValue.h"
#include "LootAction.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "RandomItemMgr.h"
#include "StatsWeightCalculator.h"
#include "Playerbots.h"

bool LootRollAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    std::vector<Roll*> rolls = group->GetRolls();
    bool voted = false;
    for (Roll*& roll : rolls)
    {
        auto voteItr = roll->playerVote.find(bot->GetGUID());
        if (voteItr == roll->playerVote.end() || voteItr->second != NOT_EMITED_YET)
            continue;

        ObjectGuid guid = roll->itemGUID;
        uint32 itemId = roll->itemid;
        int32 randomProperty = 0;
        if (roll->itemRandomPropId)
            randomProperty = roll->itemRandomPropId;
        else if (roll->itemRandomSuffix)
            randomProperty = -((int)roll->itemRandomSuffix);

        RollVote vote = PASS;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        std::string itemUsageParam;
        if (randomProperty != 0)
            itemUsageParam = std::to_string(itemId) + "," + std::to_string(randomProperty);
        else
            itemUsageParam = std::to_string(itemId);

        ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemUsageParam);

        // Armor Tokens are classed as MISC JUNK (Class 15, Subclass 0), luckily no other items I found have class bits and epic quality.
        if (proto->Class == ITEM_CLASS_MISC && proto->SubClass == ITEM_SUBCLASS_JUNK && proto->Quality == ITEM_QUALITY_EPIC)
        {
            if (CanBotUseToken(proto, bot))
                vote = NEED; // Eligible for "Need"
            else
                vote = GREED; // Not eligible, so "Greed"
        }
        else if (usage == ITEM_USAGE_DISENCHANT)
            vote = sPlayerbotAIConfig.lootRollDisenchant ? DISENCHANT : GREED;
        else
        {
            switch (proto->Class)
            {
                case ITEM_CLASS_WEAPON:
                case ITEM_CLASS_ARMOR:
                    if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_BAD_EQUIP)
                        vote = NEED;
                    else if (IsUpgradeSoon(proto, randomProperty))
                        vote = NEED;
                    else if (usage != ITEM_USAGE_NONE)
                        vote = GREED;
                    break;
                case ITEM_CLASS_RECIPE:
                    if (!sPlayerbotAIConfig.lootRollRecipe)
                        vote = PASS;
                    else if (usage == ITEM_USAGE_SKILL)
                        vote = NEED;  // Bot can learn this recipe
                    else if (proto->Bonding != BIND_WHEN_PICKED_UP)
                        vote = GREED;  // BoE recipe bot can't learn - GREED for AH/trade
                    break;
                default:
                    if (StoreLootAction::IsLootAllowed(itemId, botAI))
                        vote = CalculateRollVote(proto, usage);
                    break;
            }
        }
        if (vote == NEED)
        {
            if (sPlayerbotAIConfig.lootNeedRollLevel == 0 || RollUniqueCheck(proto, bot))
                vote = PASS;
            else if (sPlayerbotAIConfig.lootNeedRollLevel == 1)
                vote = GREED;
        }
        else if (vote == GREED && !sPlayerbotAIConfig.lootGreedRollLevel)
            vote = PASS;

        // Gear rolls are traced with the evidence behind them. A self bot greeded a one-hander that
        // outscored the one she was holding on every stat, and nothing recorded whether the item was
        // judged unusable, not an upgrade, or an upgrade that a later rule downgraded.
        if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
        {
            StatsWeightCalculator calculator(bot);
            calculator.SetItemSetBonus(false);
            calculator.SetOverflowPenalty(false);

            uint8 const slot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
            Item* const current = slot != NULL_SLOT ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) : nullptr;

            LOG_DEBUG("playerbots",
                      "[Roll] {} weighs {} ({}): usable={} slot={} score={:.1f} vs {} ({}) score={:.1f}, "
                      "usage={} -> vote {}",
                      bot->GetName(), proto->Name1, itemId, uint32(bot->BotCanUseItem(proto)), uint32(slot),
                      calculator.CalculateItem(itemId, randomProperty), current ? current->GetTemplate()->Name1 : "nothing",
                      current ? current->GetEntry() : 0,
                      current ? calculator.CalculateItem(current->GetEntry(), current->GetItemRandomPropertyId()) : 0.0f,
                      uint32(usage), uint32(vote));
        }

        switch (group->GetLootMethod())
        {
            case MASTER_LOOT:
            case FREE_FOR_ALL:
                sBotRollMgr.QueueRoll(bot, guid, PASS);
                break;
            default:
                sBotRollMgr.QueueRoll(bot, guid, vote);
                break;
        }
        voted = true;
    }

    return voted;
}

/**
 * Gear this bot cannot wear yet but will within a few levels, and that beats what it wears now.
 *
 * Item usage only counts what can be equipped this instant, so anything with a higher required level
 * is scored as vendor or auction value and greeded. A level 19 priest greeded Odo's Ley Staff -- a
 * level 21 blue with twelve spirit over her level 13 green -- and watched it go to someone else. The
 * game lets a character need on gear its class can use regardless of level; this lets a bot do what
 * a player would.
 */
bool LootRollAction::IsUpgradeSoon(ItemTemplate const* proto, int32 randomProperty)
{
    uint32 const level = bot->GetLevel();
    if (!sPlayerbotAIConfig.lootNeedLevelLookahead || proto->RequiredLevel <= level ||
        proto->RequiredLevel > level + sPlayerbotAIConfig.lootNeedLevelLookahead)
        return false;

    // Everything CanUseItem checks except the level: class, race, faction, proficiency.
    if ((proto->AllowableClass & bot->getClassMask()) == 0 || (proto->AllowableRace & bot->getRaceMask()) == 0)
        return false;
    if (proto->RequiredSkill && bot->GetSkillValue(proto->RequiredSkill) < proto->RequiredSkillRank)
        return false;
    if (proto->RequiredSpell && !bot->HasSpell(proto->RequiredSpell))
        return false;
    if (proto->GetSkill() && !bot->GetSkillValue(proto->GetSkill()))
        return false;
    if (proto->Class == ITEM_CLASS_WEAPON && !sRandomItemMgr.CanEquipWeapon(proto, bot->getClass()))
        return false;
    if (proto->Class == ITEM_CLASS_ARMOR && !sRandomItemMgr.CanEquipArmor(proto, bot->getClass(), proto->RequiredLevel, false))
        return false;

    uint8 const slot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
    if (slot == NULL_SLOT)
        return false;

    StatsWeightCalculator calculator(bot);
    calculator.SetItemSetBonus(false);
    calculator.SetOverflowPenalty(false);

    float const candidate = calculator.CalculateItem(proto->ItemId, randomProperty);
    if (candidate <= 0.0f)
        return false;

    Item* const current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!current)
        return true;

    return candidate > calculator.CalculateItem(current->GetEntry(), current->GetItemRandomPropertyId()) *
                           sPlayerbotAIConfig.equipUpgradeThreshold;
}

RollVote LootRollAction::CalculateRollVote(ItemTemplate const* proto, ItemUsage usage)
{
    if (usage == ITEM_USAGE_NONE)
    {
        std::ostringstream out;
        out << proto->ItemId;
        usage = AI_VALUE2(ItemUsage, "item usage", out.str());
    }

    RollVote needVote = PASS;
    switch (usage)
    {
        case ITEM_USAGE_EQUIP:
        case ITEM_USAGE_REPLACE:
        case ITEM_USAGE_GUILD_TASK:
        case ITEM_USAGE_BAD_EQUIP:
            needVote = NEED;
            break;
        case ITEM_USAGE_SKILL:
        case ITEM_USAGE_USE:
        case ITEM_USAGE_AH:
        case ITEM_USAGE_VENDOR:
            needVote = GREED;
            break;
        case ITEM_USAGE_DISENCHANT:
            needVote = sPlayerbotAIConfig.lootRollDisenchant ? DISENCHANT : GREED;
            break;
        default:
            break;
    }

    return StoreLootAction::IsLootAllowed(proto->ItemId, GET_PLAYERBOT_AI(bot)) ? needVote : PASS;
}

bool MasterLootRollAction::isUseful() { return !IsRealPlayer(botAI->GetMaster()); }

bool MasterLootRollAction::Execute(Event event)
{
    Player* bot = QueryItemUsageAction::botAI->GetBot();

    WorldPacket p(event.getPacket());  // WorldPacket packet for CMSG_LOOT_ROLL, (8+4+1)
    ObjectGuid creatureGuid;
    uint32 mapId;
    uint32 itemSlot;
    uint32 itemId;
    uint32 randomSuffix;
    uint32 randomPropertyId;
    uint32 count;
    uint32 timeout;

    p.rpos(0);              // reset packet pointer
    p >> creatureGuid;      // creature guid what we're looting
    p >> mapId;             /// 3.3.3 mapid
    p >> itemSlot;          // the itemEntryId for the item that shall be rolled for
    p >> itemId;            // the itemEntryId for the item that shall be rolled for
    p >> randomSuffix;      // randomSuffix
    p >> randomPropertyId;  // item random property ID
    p >> count;             // items in stack
    p >> timeout;           // the countdown time to choose "need" or "greed"

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    sBotRollMgr.QueueRoll(bot, creatureGuid, CalculateRollVote(proto));

    return true;
}

bool CanBotUseToken(ItemTemplate const* proto, Player* bot)
{
    // Get the bitmask for the bot's class
    uint32 botClassMask = (1 << (bot->getClass() - 1));

    // Check if the bot's class is allowed to use the token
    if (proto->AllowableClass & botClassMask)
        return true; // Bot's class is eligible to use this token

    return false; // Bot's class cannot use this token
}

bool RollUniqueCheck(ItemTemplate const* proto, Player* bot)
{
    // Count the total number of the item (equipped + in bags)
    uint32 totalItemCount = bot->GetItemCount(proto->ItemId, true);

    // Count the number of the item in bags only
    uint32 bagItemCount = bot->GetItemCount(proto->ItemId, false);

    // Determine if the unique item is already equipped
    bool isEquipped = (totalItemCount > bagItemCount);
    if (isEquipped && proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE))
        return true;  // Unique Item is already equipped
    else if (proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE) && (bagItemCount > 1))
        return true; // Unique item already in bag, don't roll for it
    return false; // Item is not equipped or in bags, roll for it
}

bool RollAction::Execute(Event event)
{
    std::string link = event.getParam();

    if (link.empty())
    {
        bot->DoRandomRoll(0,100);
        return false;
    }
    ItemIds itemIds = chat->parseItems(link);
    if (itemIds.empty())
        return false;
    uint32 itemId = *itemIds.begin();
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
    {
        return false;
    }
    std::string itemUsageParam;
    itemUsageParam = std::to_string(itemId);

    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemUsageParam);
    switch (proto->Class)
    {
        case ITEM_CLASS_WEAPON:
        case ITEM_CLASS_ARMOR:
        if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_BAD_EQUIP)
        {
            bot->DoRandomRoll(0,100);
        }
    }
    return true;
}
