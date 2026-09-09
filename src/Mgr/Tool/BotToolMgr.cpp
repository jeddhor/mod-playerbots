/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotToolMgr.h"

#include "BudgetValues.h"
#include "DatabaseEnv.h"
#include "Field.h"
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

namespace
{
constexpr uint32 TOOL_INTERVAL_MS = 60 * 1000;
}  // namespace

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
    if (missing.empty())
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->GetAiObjectContext())
        return;

    uint32 budget = std::min(bot->GetMoney(),
                             botAI->GetAiObjectContext()
                                 ->GetValue<uint32>("free money for", std::to_string(uint32(NeedMoneyFor::tradeskill)))
                                 ->Get());

    for (uint32 category : missing)
    {
        uint32 const entry = VendorToolFor(category);
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
