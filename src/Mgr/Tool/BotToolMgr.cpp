/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotToolMgr.h"

#include "BudgetValues.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QueryResult.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>
#include <vector>

namespace
{
constexpr uint32 TOOL_INTERVAL_MS = 60 * 1000;

/// The plain Fishing Pole every supplies vendor stocks.
constexpr uint32 FISHING_POLE = 6256;

bool IsFishingPole(Item const* item)
{
    if (!item)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    return proto && proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
}
}  // namespace

bool BotToolMgr::HasFishingPole(Player* bot)
{
    if (!bot)
        return false;

    if (IsFishingPole(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)))
        return true;

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (IsFishingPole(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            return true;

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* pBag = bot->GetBagByPos(bag))
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                if (IsFishingPole(pBag->GetItemByPos(j)))
                    return true;

    return false;
}

void BotToolMgr::EnsureLoaded()
{
    std::call_once(_loadOnce, [this]()
    {
        // Only what a vendor genuinely stocks. item_template lists a BuyPrice for plenty of things
        // no merchant sells -- every runed rod has one -- and treating that as "purchasable" would
        // conjure rods out of nothing and quietly remove the reason enchanters need blacksmiths.
        std::unordered_set<uint32> stocked;
        if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor"))
        {
            do
            {
                stocked.insert(result->Fetch()[0].Get<uint32>());
            } while (result->NextRow());
        }

        for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            if (!proto.TotemCategory || !proto.BuyPrice)
                continue;

            if (!stocked.count(entry))
                continue;

            auto itr = _vendorTools.find(proto.TotemCategory);
            if (itr == _vendorTools.end() || proto.BuyPrice < itr->second.second)
                _vendorTools[proto.TotemCategory] = {entry, proto.BuyPrice};
        }

        LOG_INFO("server.loading", ">> Indexed vendor tools for {} totem categories", _vendorTools.size());
    });
}

uint32 BotToolMgr::VendorToolFor(uint32 totemCategory)
{
    EnsureLoaded();

    auto itr = _vendorTools.find(totemCategory);
    return itr == _vendorTools.end() ? 0 : itr->second.first;
}

bool BotToolMgr::IsNeededTool(Player* bot, uint32 totemCategory)
{
    if (!totemCategory)
        return false;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        for (uint32 category : info->TotemCategory)
            if (category == totemCategory)
                return true;
    }

    return false;
}

std::unordered_set<uint32> BotToolMgr::MissingToolCategories(Player* bot)
{
    std::unordered_set<uint32> missing;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        for (uint32 category : info->TotemCategory)
        {
            if (!category)
                continue;

            // The core's own test, so a bot that is carrying something unusual but valid -- a
            // Gnomish Army Knife instead of a skinning knife -- is not sent to buy a duplicate.
            if (bot->HasItemTotemCategory(category))
                continue;

            missing.insert(category);
        }
    }

    return missing;
}

void BotToolMgr::Update(Player* bot, uint32 diff)
{
    if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
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

        timer = TOOL_INTERVAL_MS;
    }

    std::unordered_set<uint32> const missing = MissingToolCategories(bot);

    // A fisherman without a pole. Fishing checks the equipped weapon rather than a totem category,
    // so the loop below never saw it: bots with the skill went fishing empty-handed, whispered "I
    // don't have a Fishing Pole" at their owner every cast, and fished anyway.
    bool const wantsPole = bot->GetSkillValue(SKILL_FISHING) && !HasFishingPole(bot);

    if (missing.empty() && !wantsPole)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->GetAiObjectContext())
        return;

    uint32 budget = std::min(bot->GetMoney(),
                             botAI->GetAiObjectContext()
                                 ->GetValue<uint32>("free money for", std::to_string(uint32(NeedMoneyFor::tradeskill)))
                                 ->Get());

    // Zero stands for the pole, which has no category of its own.
    std::vector<uint32> wanted(missing.begin(), missing.end());
    if (wantsPole)
        wanted.push_back(0);

    for (uint32 category : wanted)
    {
        uint32 const entry = category ? VendorToolFor(category) : FISHING_POLE;
        if (!entry)
        {
            // A rod, or anything else only players make. Not this manager's problem.
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_noVendor;
            continue;
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
            continue;

        if (proto->BuyPrice > budget)
        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_unaffordable;
            continue;
        }

        // Room has to exist before the money moves, or a bot with full bags pays for nothing.
        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, 1) != EQUIP_ERR_OK)
            continue;

        bot->ModifyMoney(-int32(proto->BuyPrice));
        Item* item = bot->StoreNewItem(dest, entry, true);

        if (!item)
        {
            bot->ModifyMoney(int32(proto->BuyPrice));
            continue;
        }

        budget -= proto->BuyPrice;

        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_bought;
            _spent += proto->BuyPrice;
        }

        LOG_DEBUG("playerbots", "[Tool] {} bought {} ({}) for {}c to satisfy tool category {}", bot->GetName(),
                  proto->Name1, entry, proto->BuyPrice, category);
    }
}

std::string BotToolMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("tools: {} bought for {}c, {} unaffordable, {} needing a crafter", _bought, _spent,
                               _unaffordable, _noVendor);
}
