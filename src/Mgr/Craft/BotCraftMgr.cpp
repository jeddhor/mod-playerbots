/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotCraftMgr.h"

#include "BotEconomyMgr.h"
#include "BudgetValues.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace
{
constexpr uint32 CRAFT_INTERVAL_MS = 5 * 60 * 1000;

/// Stock the market to this depth and no further. Enough that an enchanter looking for a rod finds
/// one; few enough that blacksmiths do not spend their day making copper rods nobody wants.
constexpr uint32 TARGET_LISTING_DEPTH = 3;
}  // namespace

namespace
{
/// The skill line a crafting spell belongs to, or 0.
uint32 SkillLineOf(uint32 spellId)
{
    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
        if (itr->second && itr->second->SkillLine)
            return itr->second->SkillLine;

    return 0;
}
}  // namespace

void BotCraftMgr::EnsureLoaded()
{
    std::call_once(_loadOnce, [this]()
    {
        // First pass: what makes what.
        std::unordered_map<uint32, uint32> createdBy;
        std::vector<std::pair<uint32, uint32>> toolRecipes;  // spell, the tool it makes

        for (uint32 spellId = 1; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info)
                continue;

            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                uint32 const made = info->Effects[i].ItemType;
                if (info->Effects[i].Effect != SPELL_EFFECT_CREATE_ITEM || !made)
                    continue;

                createdBy.emplace(made, spellId);

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(made);
                if (proto && proto->TotemCategory)
                    toolRecipes.emplace_back(spellId, made);
            }
        }

        // Second pass: a blank is a tool recipe's reagent that some *other* crafting profession
        // makes.
        //
        // That cross-profession step is the whole point. An enchanter can produce its own dust and a
        // miner smelts its own bars, so neither creates a reason for two bots to trade. A rod does:
        // enchanting needs it, only blacksmithing makes it. Selecting on "reagent of a tool recipe"
        // alone swept in copper bars and had blacksmiths stocking the market with those instead.
        for (auto const& [spellId, tool] : toolRecipes)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            uint32 const toolSkill = SkillLineOf(spellId);

            for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (info->Reagent[i] <= 0)
                    continue;

                uint32 const reagent = uint32(info->Reagent[i]);
                auto maker = createdBy.find(reagent);
                if (maker == createdBy.end())
                    continue;

                uint32 const makerSkill = SkillLineOf(maker->second);
                if (!makerSkill || makerSkill == toolSkill)
                    continue;

                if (!PlayerbotFactory::IsCraftingTradeSkill(makerSkill))
                    continue;

                _toolBlanks.insert(reagent);
            }
        }

        LOG_INFO("server.loading", ">> Indexed {} cross-profession tool blanks (rods and the like)",
                 _toolBlanks.size());
    });
}

bool BotCraftMgr::IsToolBlank(uint32 itemId)
{
    EnsureLoaded();
    return _toolBlanks.count(itemId) != 0;
}

uint32 BotCraftMgr::ReagentReserve(Player* bot, uint32 itemId)
{
    // Enough for a handful of crafts, not a hoard. A bot that keeps everything never supplies the
    // market; one that keeps nothing cannot practise its own trade.
    constexpr uint32 RESERVE_CRAFTS = 5;

    uint32 perCraft = 0;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        // Only recipes the bot could actually perform: a spell it knows but has no skill for
        // reserves nothing.
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (info->Reagent[i] > 0 && uint32(info->Reagent[i]) == itemId)
                perCraft = std::max(perCraft, info->ReagentCount[i]);
    }

    return perCraft * RESERVE_CRAFTS;
}

uint32 BotCraftMgr::RefineMaterials(Player* bot)
{
    EnsureLoaded();

    uint32 made = 0;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        // Smelting and its equivalents: a gathering profession's recipe that turns what was picked
        // up into what is actually traded. Ore is worth less than the bar it becomes, and a bot
        // that never smelts floods the house with ore nobody wants.
        uint32 const skill = SkillLineOf(spellId);
        if (!skill || !PlayerbotFactory::IsGatheringTradeSkill(skill))
            continue;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 const product = info->Effects[i].ItemType;
            if (info->Effects[i].Effect != SPELL_EFFECT_CREATE_ITEM || !product)
                continue;

            // Up to a few per pass so a full stack of ore is worked through over a few minutes
            // rather than all at once.
            for (uint32 attempt = 0; attempt < 5; ++attempt)
            {
                if (!CraftOne(bot, spellId, product, false))
                    break;

                ++made;
            }
        }
    }

    return made;
}

bool BotCraftMgr::BuyReagent(Player* bot, uint32 itemId, uint32 needed)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->GetAiObjectContext())
        return false;

    uint32 const budget = std::min(bot->GetMoney(),
                                   botAI->GetAiObjectContext()
                                       ->GetValue<uint32>("free money for",
                                                          std::to_string(uint32(NeedMoneyFor::tradeskill)))
                                       ->Get());

    if (!budget)
        return false;

    for (BotEconomyMgr::Bargain const& bargain : sBotEconomyMgr.SampleBargains(bot->GetTeamId(), 60))
    {
        if (bargain.itemId != itemId || bargain.owner == bot->GetGUID())
            continue;

        if (!bargain.buyout || bargain.buyout > budget)
            continue;

        if (sBotEconomyMgr.BuyoutAuction(bot, bargain.auctionId, bargain.houseId))
        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_reagentsBought;

            LOG_DEBUG("playerbots", "[Craft] {} bought {} x{} off the auction house for {}c", bot->GetName(), itemId,
                      bargain.count, bargain.buyout);
            return true;
        }
    }

    return false;
}

bool BotCraftMgr::CraftOne(Player* bot, uint32 spellId, uint32 itemId, bool forMarket)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return false;

    // Everything the recipe wants must be present before anything is consumed, or a bot short of
    // one reagent destroys the others for nothing.
    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (info->Reagent[i] <= 0 || !info->ReagentCount[i])
            continue;

        if (bot->GetItemCount(uint32(info->Reagent[i]), false) < info->ReagentCount[i])
        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_shortReagents;
            return false;
        }
    }

    ItemPosCountVec dest;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) != EQUIP_ERR_OK)
        return false;

    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        if (info->Reagent[i] > 0 && info->ReagentCount[i])
            bot->DestroyItemCount(uint32(info->Reagent[i]), info->ReagentCount[i], true);

    Item* made = bot->StoreNewItem(dest, itemId, true);
    if (!made)
        return false;

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_crafted;
    }

    // A blank made to supply the market goes straight to the house. A smelted bar does not: the bot
    // may want it, and whether the surplus is worth listing is the economy's decision, made against
    // the reserve rather than here.
    if (forMarket && sBotEconomyMgr.PostAuction(bot, made))
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_listed;
    }

    LOG_DEBUG("playerbots", "[Craft] {} made {} for the market", bot->GetName(), itemId);
    return true;
}

void BotCraftMgr::Update(Player* bot, uint32 diff)
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

        timer = CRAFT_INTERVAL_MS;
    }

    EnsureLoaded();

    // Refine before supplying: the bars a smelter just made may be exactly what its rod recipe
    // wanted, and there is no sense buying what is already in the bag as ore.
    RefineMaterials(bot);

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 const itemId = info->Effects[i].ItemType;
            if (info->Effects[i].Effect != SPELL_EFFECT_CREATE_ITEM || !itemId)
                continue;

            if (!_toolBlanks.count(itemId))
                continue;

            // Demand-led. Supplying a market that is already stocked is how a realm ends up with
            // four hundred copper rods and no adamantite ones.
            if (sBotEconomyMgr.GetListingDepth(itemId) >= TARGET_LISTING_DEPTH)
                continue;

            if (CraftOne(bot, spellId, itemId, true))
                return;  // one per pass, so a single bot does not corner the whole tool market

            // Short a reagent. Buying it is the point of having an auction house: a blacksmith who
            // cannot mine can still make rods if some miner listed the bars.
            for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            {
                if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
                    continue;

                uint32 const reagent = uint32(info->Reagent[r]);
                if (bot->GetItemCount(reagent, false) >= info->ReagentCount[r])
                    continue;

                if (BuyReagent(bot, reagent, info->ReagentCount[r]))
                    return;
            }
        }
    }
}

std::string BotCraftMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("craft: {} made, {} listed, {} passes short of reagents", _crafted, _listed,
                               _shortReagents);
}
