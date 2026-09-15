/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotCraftMgr.h"

#include "Bag.h"
#include "BotEconomyMgr.h"
#include "BudgetValues.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "StringFormat.h"
#include "World.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace
{
constexpr uint32 CRAFT_INTERVAL_MS = 5 * 60 * 1000;

/// Stock the market to this depth and no further. Enough that an enchanter looking for a rod finds
/// one; few enough that blacksmiths do not spend their day making copper rods nobody wants.
constexpr uint32 TARGET_LISTING_DEPTH = 3;

/// An upgrade has to beat what is worn by this fraction before it is worth buying materials for.
constexpr float UPGRADE_MARGIN = 0.05f;
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

/// Every reagent present in the quantity the recipe wants.
bool HasAllReagents(Player* bot, SpellInfo const* info)
{
    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (info->Reagent[i] <= 0 || !info->ReagentCount[i])
            continue;

        if (bot->GetItemCount(uint32(info->Reagent[i]), false) < info->ReagentCount[i])
            return false;
    }

    return true;
}

/// The item a crafting spell produces, or 0 if it makes nothing.
uint32 CreatedItemOf(SpellInfo const* info)
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (info->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[i].ItemType)
            return info->Effects[i].ItemType;

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
                if (info->Effects[i].Effect == SPELL_EFFECT_MILLING)
                    _millingSpells.insert(spellId);

                // An enchant that names an item writes a scroll of itself when cast on vellum.
                ItemTemplate const* written = info->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM
                                                  ? sObjectMgr->GetItemTemplate(info->Effects[i].ItemType)
                                                  : nullptr;
                if (written && written->Class == ITEM_CLASS_CONSUMABLE)
                {
                    _scrollSpell.emplace(info->Effects[i].ItemType, spellId);
                    _spellScroll.emplace(spellId, info->Effects[i].ItemType);
                }

                uint32 const made = info->Effects[i].ItemType;
                if (info->Effects[i].Effect != SPELL_EFFECT_CREATE_ITEM || !made)
                    continue;

                createdBy.emplace(made, spellId);

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(made);
                if (proto && proto->TotemCategory)
                    toolRecipes.emplace_back(spellId, made);

                // Vellum: consumed by enchanting, made only by inscription. The same cross-profession
                // dependency a rod is, which is why it goes in the same supply set.
                if (proto && (proto->IsArmorVellum() || proto->IsWeaponVellum()))
                    _toolBlanks.insert(made);
            }
        }

        for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            if (proto.IsArmorVellum())
                _armorVellums.emplace_back(proto.ItemLevel, entry);
            else if (proto.IsWeaponVellum())
                _weaponVellums.emplace_back(proto.ItemLevel, entry);

            if (proto.HasFlag(ITEM_FLAG_IS_MILLABLE) && LootTemplates_Milling.HaveLootFor(entry))
                _millable.insert(entry);
        }

        std::sort(_armorVellums.begin(), _armorVellums.end());
        std::sort(_weaponVellums.begin(), _weaponVellums.end());

        // Parchment is the case: every vellum needs it and only a vendor has it. Extended-cost rows are
        // excluded, since those trade in tokens -- the Dalaran ink trader's inks among them -- not gold.
        if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor WHERE ExtendedCost = 0"))
            do
            {
                uint32 const entry = result->Fetch()[0].Get<uint32>();
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
                if (proto && proto->BuyPrice && proto->Class == ITEM_CLASS_TRADE_GOODS)
                    _vendorGoods.insert(entry);
            } while (result->NextRow());

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

        LOG_INFO("server.loading", ">> Indexed {} cross-profession supply items (rods, vellum and the like)",
                 _toolBlanks.size());
        LOG_INFO("server.loading",
                 ">> Indexed {} enchant scrolls, {} armor and {} weapon vellums, {} millable herbs, {} vendor goods",
                 _scrollSpell.size(), _armorVellums.size(), _weaponVellums.size(), _millable.size(),
                 _vendorGoods.size());
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

    // Nothing on the house. A vendor good is still a vendor good: parchment is never going to be
    // listed by a bot, and a scribe must not wait on an auction house for something any supplier has.
    return BuyFromVendor(bot, itemId, needed, budget);
}

bool BotCraftMgr::BuyFromVendor(Player* bot, uint32 itemId, uint32 needed, uint32 budget)
{
    if (!_vendorGoods.count(itemId))
        return false;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto || !proto->BuyPrice)
        return false;

    uint32 const perPurchase = std::max<uint32>(1, proto->BuyCount);
    uint32 const held = bot->GetItemCount(itemId, false);
    uint32 const missing = needed > held ? needed - held : 0;
    if (!missing)
        return false;

    uint32 const purchases = (missing + perPurchase - 1) / perPurchase;
    uint32 const count = purchases * perPurchase;
    uint32 const price = purchases * proto->BuyPrice;
    if (price > budget || price > bot->GetMoney())
        return false;

    ItemPosCountVec dest;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) != EQUIP_ERR_OK)
        return false;

    Item* bought = bot->StoreNewItem(dest, itemId, true);
    if (!bought)
        return false;

    bot->ModifyMoney(-int32(price));
    bot->SendNewItem(bought, count, true, false);

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_vendorBought;
    }

    LOG_DEBUG("playerbots", "[Craft] {} bought {} x{} from a vendor for {}c", bot->GetName(), itemId, count, price);
    return true;
}

bool BotCraftMgr::CraftReagent(Player* bot, uint32 itemId, bool dryRun)
{
    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || CreatedItemOf(info) != itemId || !HasAllReagents(bot, info))
            continue;

        if (dryRun || CraftOne(bot, spellId, itemId, false))
            return true;
    }

    return false;
}

bool BotCraftMgr::IsObtainable(Player* bot, uint32 itemId, uint32 needed)
{
    return bot->GetItemCount(itemId, false) >= needed || _vendorGoods.count(itemId) ||
           sBotEconomyMgr.GetListingDepth(itemId) > 0 || CraftReagent(bot, itemId, true);
}

bool BotCraftMgr::AllReagentsObtainable(Player* bot, SpellInfo const* info)
{
    for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
        if (info->Reagent[r] > 0 && info->ReagentCount[r] &&
            !IsObtainable(bot, uint32(info->Reagent[r]), info->ReagentCount[r]))
            return false;

    return true;
}

uint32 BotCraftMgr::MillHerbs(Player* bot)
{
    constexpr uint32 HERBS_PER_MILL = 5;
    constexpr uint32 MILLS_PER_PASS = 4;

    if (_millingSpells.empty() || !bot->HasSkill(SKILL_INSCRIPTION))
        return 0;

    // Milling is a skill-line reward, re-granted from the Inscription skill on every load rather
    // than stored, so asking the spell map is the only honest test of whether this scribe has it.
    bool knowsMilling = false;
    for (uint32 spellId : _millingSpells)
        if (bot->HasSpell(spellId))
        {
            knowsMilling = true;
            break;
        }

    if (!knowsMilling)
        return 0;

    uint16 const skill = bot->GetSkillValue(SKILL_INSCRIPTION);
    uint32 milled = 0;

    for (uint32 herb : _millable)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(herb);
        if (!proto || proto->RequiredSkillRank > skill)
            continue;

        while (milled < MILLS_PER_PASS && bot->GetItemCount(herb, false) >= HERBS_PER_MILL)
        {
            Loot loot;
            loot.FillLoot(herb, LootTemplates_Milling, bot, true, true);

            // Room for everything before anything is destroyed, or a full bag eats the herbs.
            bool fits = !loot.items.empty();
            for (LootItem const& drop : loot.items)
            {
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, drop.itemid, drop.count) != EQUIP_ERR_OK)
                {
                    fits = false;
                    break;
                }
            }

            if (!fits)
                return milled;

            bot->DestroyItemCount(herb, HERBS_PER_MILL, true);

            for (LootItem const& drop : loot.items)
            {
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, drop.itemid, drop.count) != EQUIP_ERR_OK)
                    continue;

                if (Item* pigment = bot->StoreNewItem(dest, drop.itemid, true, drop.randomPropertyId))
                    bot->SendNewItem(pigment, drop.count, true, false);
            }

            // The skill-up a real mill grants, subject to the same realm switch the core honours.
            if (sWorld->getBoolConfig(CONFIG_SKILL_MILLING))
                bot->UpdateGatherSkill(SKILL_INSCRIPTION, bot->GetPureSkillValue(SKILL_INSCRIPTION),
                                       proto->RequiredSkillRank);

            ++milled;
        }

        if (milled >= MILLS_PER_PASS)
            break;
    }

    if (milled)
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _milled += milled;
        LOG_DEBUG("playerbots", "[Craft] {} milled {} batches of herbs", bot->GetName(), milled);
    }

    return milled;
}

uint32 BotCraftMgr::VellumFor(Player* bot, SpellInfo const* enchant)
{
    if (!enchant)
        return 0;

    // Spell::CheckCast: the target's required level, or its item level when it has none, must reach
    // the enchant's base level. Vellum has no required level, so its item level decides the tier.
    //
    // The cheapest acceptable tier the bot can actually get. Any higher tier also takes the enchant,
    // so a house holding only Vellum III still serves a low enchant -- whether that is worth doing is
    // the margin test's call, not this one's.
    auto const& tiers = enchant->EquippedItemClass == ITEM_CLASS_WEAPON ? _weaponVellums : _armorVellums;
    for (auto const& [itemLevel, entry] : tiers)
        if (itemLevel >= enchant->BaseLevel && IsObtainable(bot, entry, 1))
            return entry;

    return 0;
}

Item* BotCraftMgr::ScrollTarget(Player* bot, SpellInfo const* enchant) const
{
    if (!enchant)
        return nullptr;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->IsFitToSpellRequirements(enchant))
            continue;

        // Empty slots only. Bots are the sink for modest enchants; overwriting one a player chose, or a
        // better one the bot already has, is not what a bought scroll is for.
        if (item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT))
            continue;

        ItemTemplate const* proto = item->GetTemplate();
        if (!enchant->HasAttribute(SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF))
        {
            uint32 const requiredLevel = proto->RequiredLevel ? proto->RequiredLevel : proto->ItemLevel;
            if (requiredLevel < enchant->BaseLevel)
                continue;
        }

        if (enchant->MaxLevel > 0 && proto->ItemLevel > enchant->MaxLevel)
            continue;

        return item;
    }

    return nullptr;
}

bool BotCraftMgr::IsScrollUsefulTo(Player* bot, uint32 itemId)
{
    if (!bot || !sPlayerbotAIConfig.scrollTradeEnabled)
        return false;

    EnsureLoaded();

    auto const itr = _scrollSpell.find(itemId);
    return itr != _scrollSpell.end() && ScrollTarget(bot, sSpellMgr->GetSpellInfo(itr->second));
}

bool BotCraftMgr::IsHerbToMill(Player* bot, uint32 itemId)
{
    if (!bot || !sPlayerbotAIConfig.scrollTradeEnabled || !bot->HasSkill(SKILL_INSCRIPTION))
        return false;

    EnsureLoaded();

    if (!_millable.count(itemId))
        return false;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    return proto && proto->RequiredSkillRank <= bot->GetSkillValue(SKILL_INSCRIPTION);
}

bool BotCraftMgr::IsVellumFor(Player* bot, uint32 itemId)
{
    if (!bot || !sPlayerbotAIConfig.scrollTradeEnabled || !bot->HasSkill(SKILL_ENCHANTING))
        return false;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    return proto && (proto->IsArmorVellum() || proto->IsWeaponVellum());
}

bool BotCraftMgr::SupplyScroll(Player* bot)
{
    if (!sPlayerbotAIConfig.scrollTradeEnabled || !bot->HasSkill(SKILL_ENCHANTING))
        return false;

    uint16 const skill = bot->GetSkillValue(SKILL_ENCHANTING);

    uint32 bestSpell = 0;
    uint32 bestScroll = 0;
    uint32 bestVellum = 0;
    int64 bestMargin = 0;

    uint32 considered = 0;
    uint32 noVellum = 0;
    uint32 noReagents = 0;
    uint32 noMargin = 0;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        auto const scrollItr = _spellScroll.find(spellId);
        if (scrollItr == _spellScroll.end())
            continue;

        uint32 const scroll = scrollItr->second;

        // Demand-led, like the rods: a house already holding a few of this scroll needs no more.
        if (sBotEconomyMgr.GetListingDepth(scroll) >= TARGET_LISTING_DEPTH)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        bool skilled = false;
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (itr->second && itr->second->SkillLine == SKILL_ENCHANTING && skill >= itr->second->MinSkillLineRank)
                skilled = true;

        if (!skilled)
            continue;

        ++considered;

        uint32 const vellum = VellumFor(bot, info);
        if (!vellum)
        {
            ++noVellum;
            continue;
        }

        // Same rule as the supply loop: do not start buying for a scroll that cannot be finished.
        if (!AllReagentsObtainable(bot, info))
        {
            ++noReagents;
            continue;
        }

        // Only when the scroll pays for what goes into it. Burning dust to make something worth less
        // than the dust is how an enchanter goes broke supplying a market.
        int64 cost = sBotEconomyMgr.GetMarketPrice(vellum);
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (info->Reagent[r] > 0 && info->ReagentCount[r])
                cost += int64(sBotEconomyMgr.GetMarketPrice(uint32(info->Reagent[r]))) * info->ReagentCount[r];

        int64 const margin = int64(sBotEconomyMgr.GetMarketPrice(scroll)) - cost;
        if (margin <= 0)
            ++noMargin;
        if (margin <= bestMargin)
            continue;

        bestMargin = margin;
        bestSpell = spellId;
        bestScroll = scroll;
        bestVellum = vellum;
    }

    // A scroll trade that stays quiet must say why. The first version of this ran for twenty minutes
    // beside four listed vellums and wrote nothing, and nothing in the log could tell a pricing fault
    // from an enchanter with no dust.
    if (!bestSpell)
    {
        if (considered)
            LOG_DEBUG("playerbots", "[ScrollSkip] {} considered {} scrolls: {} no vellum, {} reagents, {} no margin",
                      bot->GetName(), considered, noVellum, noReagents, noMargin);
        return false;
    }

    SpellInfo const* info = sSpellMgr->GetSpellInfo(bestSpell);

    // One purchase per pass, same as the upgrade path: the vellum first, since without it the dust
    // is not worth buying.
    if (!bot->GetItemCount(bestVellum, false))
        return BuyReagent(bot, bestVellum, 1);

    for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
    {
        if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
            continue;

        if (bot->GetItemCount(uint32(info->Reagent[r]), false) < info->ReagentCount[r])
            return BuyReagent(bot, uint32(info->Reagent[r]), info->ReagentCount[r]);
    }

    ItemPosCountVec dest;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, bestScroll, 1) != EQUIP_ERR_OK)
        return false;

    for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
        if (info->Reagent[r] > 0 && info->ReagentCount[r])
            bot->DestroyItemCount(uint32(info->Reagent[r]), info->ReagentCount[r], true);
    bot->DestroyItemCount(bestVellum, 1, true);

    Item* made = bot->StoreNewItem(dest, bestScroll, true);
    if (!made)
        return false;

    bot->SendNewItem(made, 1, false, true);

    // SpellEffects grants skill for a vellum only when the realm says so; a direct craft must agree.
    if (sWorld->getBoolConfig(CONFIG_ENCHANT_VELLUM_SKILL_GAIN))
        bot->UpdateCraftSkill(bestSpell);

    bool const listed = sBotEconomyMgr.PostAuction(bot, made);

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_scrollsWritten;
        if (listed)
            ++_listed;
    }

    LOG_DEBUG("playerbots", "[Scroll] {} wrote scroll {} (margin {}c){}", bot->GetName(), bestScroll, bestMargin,
              listed ? " and listed it" : "");
    return true;
}

uint32 BotCraftMgr::DisenchantHeld(Player* bot)
{
    constexpr uint32 DISENCHANTS_PER_PASS = 5;
    constexpr uint32 SPELL_DISENCHANT = 13262;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->GetAiObjectContext() || !bot->HasSkill(SKILL_ENCHANTING) || bot->IsInCombat())
        return 0;

    uint16 const skill = bot->GetSkillValue(SKILL_ENCHANTING);

    // Collected first and destroyed afterwards: destroying while walking the bags moves what is left.
    std::vector<std::pair<uint8, uint8>> fodder;
    auto consider = [&](Item* item, uint8 bag, uint8 slot)
    {
        if (!item || fodder.size() >= DISENCHANTS_PER_PASS)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto->DisenchantID || proto->RequiredDisenchantSkill > skill || item->IsRefundable() ||
            item->IsWrapped() || (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON))
            return;

        // The classifier owns the judgement -- upgrade, a groupmate's upgrade, keep for a set, list it --
        // and this only acts on its verdict, so the two cannot disagree about what counts as fodder.
        ItemUsage const usage = botAI->GetAiObjectContext()
                                    ->GetValue<ItemUsage>("item usage", std::to_string(proto->ItemId))
                                    ->Get();
        if (usage == ITEM_USAGE_DISENCHANT)
            fodder.emplace_back(bag, slot);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                consider(bag->GetItemByPos(uint8(slot)), bagSlot, uint8(slot));

    uint32 done = 0;
    for (auto const& [bag, slot] : fodder)
    {
        Item* item = bot->GetItemByPos(bag, slot);
        if (!item)
            continue;

        ItemTemplate const* proto = item->GetTemplate();
        uint32 const entry = proto->ItemId;

        Loot loot;
        loot.FillLoot(proto->DisenchantID, LootTemplates_Disenchant, bot, true, true);
        if (loot.items.empty())
            continue;

        // Room for every reagent before the gear is gone, or a full bag destroys it for nothing.
        bool fits = true;
        for (LootItem const& drop : loot.items)
        {
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, drop.itemid, drop.count) != EQUIP_ERR_OK)
            {
                fits = false;
                break;
            }
        }

        if (!fits)
            break;

        bot->DestroyItem(bag, slot, true);

        for (LootItem const& drop : loot.items)
        {
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, drop.itemid, drop.count) != EQUIP_ERR_OK)
                continue;

            if (Item* reagent = bot->StoreNewItem(dest, drop.itemid, true, drop.randomPropertyId))
                bot->SendNewItem(reagent, drop.count, true, false);
        }

        // Spell::EffectDisEnchant grants skill, and it matters: disenchanting is how a new enchanter
        // climbs its first sixty points.
        bot->UpdateCraftSkill(SPELL_DISENCHANT);

        ++done;
        LOG_DEBUG("playerbots", "[Disenchant] {} disenchanted {} into {} reagent stack(s)", bot->GetName(), entry,
                  loot.items.size());
    }

    if (done)
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _disenchanted += done;
    }

    return done;
}

bool BotCraftMgr::BuyToDisenchant(Player* bot)
{
    // Disenchanting teaches until this skill, so below it gear is worth a little more than its dust.
    constexpr uint16 DISENCHANT_TEACHES_UNTIL = 60;
    constexpr float LEARNING_PREMIUM = 1.5f;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->GetAiObjectContext() || !bot->HasSkill(SKILL_ENCHANTING))
        return false;

    uint16 const skill = bot->GetSkillValue(SKILL_ENCHANTING);
    uint32 const budget = std::min(bot->GetMoney(),
                                   botAI->GetAiObjectContext()
                                       ->GetValue<uint32>("free money for",
                                                          std::to_string(uint32(NeedMoneyFor::tradeskill)))
                                       ->Get());
    if (!budget)
        return false;

    float const ceilingPct = float(sPlayerbotAIConfig.economyDisenchantMaxPricePct) / 100.0f *
                             (skill < DISENCHANT_TEACHES_UNTIL ? LEARNING_PREMIUM : 1.0f);

    for (BotEconomyMgr::Bargain const& bargain : sBotEconomyMgr.SampleBargains(bot->GetTeamId(), 60))
    {
        if (bargain.owner == bot->GetGUID() || !bargain.buyout || bargain.buyout > budget)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(bargain.itemId);
        if (!proto || !proto->DisenchantID || proto->RequiredDisenchantSkill > skill ||
            proto->Quality < ITEM_QUALITY_UNCOMMON || proto->Quality > sPlayerbotAIConfig.economyDisenchantMaxQuality ||
            (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON))
            continue;

        uint32 const dustValue = sBotEconomyMgr.GetDisenchantValue(proto->DisenchantID) * bargain.count;
        if (bargain.buyout > uint32(float(dustValue) * ceilingPct))
            continue;

        if (!sBotEconomyMgr.BuyoutAuction(bot, bargain.auctionId, bargain.houseId))
            continue;

        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_boughtToDisenchant;
        }

        LOG_DEBUG("playerbots", "[Disenchant] {} bought {} for {}c to disenchant (dust worth {}c)", bot->GetName(),
                  bargain.itemId, bargain.buyout, dustValue);
        return true;
    }

    return false;
}

bool BotCraftMgr::BuyScroll(Player* bot)
{
    if (!sPlayerbotAIConfig.scrollTradeEnabled || sPlayerbotAIConfig.scrollBuyWillingness <= 0.0f)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->GetAiObjectContext())
        return false;

    uint32 const budget = std::min(bot->GetMoney(),
                                   botAI->GetAiObjectContext()
                                       ->GetValue<uint32>("free money for", std::to_string(uint32(NeedMoneyFor::gear)))
                                       ->Get());
    if (!budget)
        return false;

    for (BotEconomyMgr::Bargain const& bargain : sBotEconomyMgr.SampleBargains(bot->GetTeamId(), 60))
    {
        if (bargain.owner == bot->GetGUID() || !bargain.buyout || bargain.buyout > budget)
            continue;

        auto const itr = _scrollSpell.find(bargain.itemId);
        if (itr == _scrollSpell.end())
            continue;

        uint32 const ceiling = uint32(float(sBotEconomyMgr.GetMarketPrice(bargain.itemId)) * bargain.count *
                                      sPlayerbotAIConfig.scrollBuyWillingness);
        if (bargain.buyout > ceiling)
            continue;

        if (!ScrollTarget(bot, sSpellMgr->GetSpellInfo(itr->second)))
            continue;

        // Held scrolls are applied before anything is bought, so one still in the bag or the mail is
        // for some other slot -- but a second copy of the same scroll never is.
        if (bot->GetItemCount(bargain.itemId, true))
            continue;

        if (!sBotEconomyMgr.BuyoutAuction(bot, bargain.auctionId, bargain.houseId))
            continue;

        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_scrollsBought;
        }

        LOG_DEBUG("playerbots", "[Scroll] {} bought scroll {} for {}c", bot->GetName(), bargain.itemId,
                  bargain.buyout);
        return true;
    }

    return false;
}

uint32 BotCraftMgr::ApplyScrolls(Player* bot)
{
    if (!sPlayerbotAIConfig.scrollTradeEnabled)
        return 0;

    uint32 applied = 0;

    for (auto const& [scroll, spellId] : _scrollSpell)
    {
        if (!bot->GetItemCount(scroll, false))
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        Item* target = ScrollTarget(bot, info);
        if (!target)
            continue;

        uint32 enchantId = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM)
                enchantId = info->Effects[i].MiscValue;

        if (!enchantId || !sSpellItemEnchantmentStore.LookupEntry(enchantId))
            continue;

        // What Spell::EffectEnchantItemPerm does for a scroll used on gear, without casting: the cast
        // would need the client-side targeting a bot does not have.
        bot->ApplyEnchantment(target, PERM_ENCHANTMENT_SLOT, false);
        target->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantId, 0, 0, bot->GetGUID());
        bot->ApplyEnchantment(target, PERM_ENCHANTMENT_SLOT, true);
        target->ClearSoulboundTradeable(bot);

        bot->DestroyItemCount(scroll, 1, true);
        ++applied;

        LOG_DEBUG("playerbots", "[Scroll] {} applied scroll {} to {}", bot->GetName(), scroll,
                  target->GetTemplate()->ItemId);
    }

    if (applied)
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _scrollsApplied += applied;
    }

    return applied;
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

    // Announce it the way a real craft does. StoreNewItem puts the item in the bag but sends no
    // SMSG_ITEM_PUSH_RESULT, and that packet is what drives the bot's equip check -- without it a
    // crafted upgrade waits for the next periodic sweep instead of being worn straight away. On a
    // self bot it is also what the owner's client shows as "you made this".
    bot->SendNewItem(made, 1, false, true);

    // The spell is never actually cast here -- the reagents are destroyed and the product stored
    // directly -- so the skill-up the core grants on a real craft has to be asked for explicitly.
    // Without this a blacksmith supplies rods to the market all day and ends the day at the skill
    // it started with. UpdateCraftSkill does the colour roll itself, so a grey recipe correctly
    // yields nothing.
    bot->UpdateCraftSkill(spellId);

    LOG_DEBUG("playerbots", "[Craft] {} made {} {}", bot->GetName(), itemId,
              forMarket ? "for the market" : "for itself");
    return true;
}

bool BotCraftMgr::CraftForSkillUp(Player* bot)
{
    uint32 bestSpell = 0;
    uint32 bestItem = 0;
    uint32 bestGreyAt = 0;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        uint32 const itemId = CreatedItemOf(info);
        if (!itemId)
            continue;

        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineAbilityEntry const* ability = itr->second;
            if (!ability || !ability->SkillLine || !ability->TrivialSkillLineRankHigh)
                continue;

            // TrivialSkillLineRankHigh is where the recipe goes grey and stops paying. At or above
            // it, crafting consumes materials for nothing, which is worse than not crafting.
            uint16 const skill = bot->GetPureSkillValue(ability->SkillLine);
            if (!skill || skill < ability->MinSkillLineRank || skill >= ability->TrivialSkillLineRankHigh)
                continue;

            // Closest to grey first. Materials should go to the recipe that is about to stop paying
            // rather than to one that will still be paying in fifty points' time -- the cheap
            // low-level recipe is the one whose remaining value is expiring.
            if (bestSpell && ability->TrivialSkillLineRankHigh >= bestGreyAt)
                continue;

            // Checked last because it is the expensive test, and only for a recipe that has already
            // earned the right to be crafted.
            if (!HasAllReagents(bot, info))
                continue;

            bestGreyAt = ability->TrivialSkillLineRankHigh;
            bestSpell = spellId;
            bestItem = itemId;
        }
    }

    if (!bestSpell)
        return false;

    // forMarket is false: the product stays with the bot, and whether it is worth listing is the
    // economy's decision made against the reserve. Food especially -- a cooked stack is what the
    // eat/drink logic wants, and auctioning it the moment it is made defeats the point of cooking.
    if (!CraftOne(bot, bestSpell, bestItem, false))
        return false;

    std::unique_lock<std::shared_mutex> guard(_mutex);
    ++_skillCrafts;
    return true;
}

bool BotCraftMgr::CraftUpgrade(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    StatsWeightCalculator calculator(bot);
    calculator.SetItemSetBonus(false);
    calculator.SetOverflowPenalty(false);

    uint32 bestSpell = 0;
    uint32 bestItem = 0;
    float bestGain = 0.0f;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        uint32 const itemId = CreatedItemOf(info);
        if (!itemId)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto || (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON))
            continue;

        // Class, race and level requirements. A tailor can make plate-wearer gear and a smith can
        // make things it cannot lift; neither is an upgrade to the bot that made it.
        if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;

        uint8 const slot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot >= EQUIPMENT_SLOT_END)
            continue;

        float const newScore = calculator.CalculateItem(itemId);
        if (newScore <= 0.0f)
            continue;

        Item* equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        float const currentScore =
            equipped ? calculator.CalculateItem(equipped->GetTemplate()->ItemId,
                                                equipped->GetItemRandomPropertyId())
                     : 0.0f;

        // A genuine upgrade, not a rounding difference. Buying a stack of ore to gain half a point
        // of attack power is how a bot spends its gold on nothing.
        float const gain = newScore - currentScore;
        if (gain <= 0.0f || gain < currentScore * UPGRADE_MARGIN)
            continue;

        if (gain <= bestGain)
            continue;

        bestGain = gain;
        bestSpell = spellId;
        bestItem = itemId;
    }

    if (!bestSpell)
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(bestSpell);
    if (!info)
        return false;

    // Spend nothing on an upgrade that cannot be finished. With vendor goods now buyable this is real
    // gold: a leatherworker was buying 15g of Heavy Knothide Leather a pass for a recipe whose other
    // reagent nobody sells or lists.
    if (!AllReagentsObtainable(bot, info))
        return false;

    // Buy one missing reagent per pass rather than the whole shopping list at once. A bot that
    // empties its purse in a single tick cannot react to what that purchase did to the price, and
    // the five-minute interval makes the trickle invisible in play.
    for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
    {
        if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
            continue;

        uint32 const reagent = uint32(info->Reagent[r]);
        if (bot->GetItemCount(reagent, false) >= info->ReagentCount[r])
            continue;

        return BuyReagent(bot, reagent, info->ReagentCount[r]);
    }

    // Nothing missing: everything is in the bag and the thing is worth making.
    if (!CraftOne(bot, bestSpell, bestItem, false))
        return false;

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_upgradeCrafts;
    }

    LOG_DEBUG("playerbots", "[Craft] {} crafted {} as an upgrade (+{:.1f})", bot->GetName(), bestItem, bestGain);
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
    // wanted, and there is no sense buying what is already in the bag as ore. Milling is the same
    // step for a scribe -- pigments first, so the skill-up pass below can turn them into ink.
    RefineMaterials(bot);
    MillHerbs(bot);

    // Anything bought last pass has arrived by mail since; use it before deciding to buy more. That
    // goes for gear bought to break down as much as for scrolls.
    ApplyScrolls(bot);
    DisenchantHeld(bot);

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

            // Nothing is bought unless every missing reagent can be had. Otherwise a scribe with no ink
            // buys this tier's parchment, stalls, moves to the next tier next pass and buys that
            // parchment too -- a bag of every parchment in the game and not one vellum.
            if (!AllReagentsObtainable(bot, info))
                continue;

            // Short a reagent. Buying it is the point of having an auction house: a blacksmith who
            // cannot mine can still make rods if some miner listed the bars.
            for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            {
                if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
                    continue;

                uint32 const reagent = uint32(info->Reagent[r]);
                if (bot->GetItemCount(reagent, false) >= info->ReagentCount[r])
                    continue;

                // Make it before buying it. A scribe past the point where ink still teaches anything
                // never inks for skill, and nobody lists ink -- so its pigments would sit in the bag
                // while the vellum that needs the ink never gets made.
                if (CraftReagent(bot, reagent) || BuyReagent(bot, reagent, info->ReagentCount[r]))
                    return;
            }
        }
    }

    // Scrolls next: written only when the house is short and the margin is real, bought only for an
    // empty enchant slot. Both are rare, and either one is this bot's action for the pass.
    if (SupplyScroll(bot) || BuyScroll(bot) || BuyToDisenchant(bot))
        return;

    // The market comes first only because it almost never fires -- it is gated on the auction house
    // actually being short of a blank.
    //
    // Then upgrades before skill-ups. An upgrade is worth buying materials for and a skill point is
    // not, so the pass that is allowed to spend gold gets first refusal on the bot's attention; if
    // it finds nothing worth making, the free pass over what is already in the bag runs instead.
    if (CraftUpgrade(bot))
        return;

    CraftForSkillUp(bot);
}

std::string BotCraftMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("craft: {} made ({} for skill, {} upgrades), {} listed, {} passes short of reagents, "
                               "{} vendor buys, {} mills; scrolls: {} written, {} bought, {} applied; "
                               "disenchant: {} done, {} bought for it",
                               _crafted, _skillCrafts, _upgradeCrafts, _listed, _shortReagents, _vendorBought,
                               _milled, _scrollsWritten, _scrollsBought, _scrollsApplied, _disenchanted,
                               _boughtToDisenchant);
}
