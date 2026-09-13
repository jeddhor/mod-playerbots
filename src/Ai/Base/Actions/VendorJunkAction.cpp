/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "VendorJunkAction.h"
#include "BotEconomyMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

namespace
{
/// Collects every item currently in the bot's bags so the caller can filter them.
class CollectBagItemsVisitor : public IterateItemsVisitor
{
public:
    bool Visit(Item* item) override
    {
        if (item)
            items.push_back(item);
        return true;
    }

    std::vector<Item*> items;
};
}  // namespace

bool VendorJunkAction::isUseful()
{
    return sPlayerbotAIConfig.autoVendorJunk;
}

bool VendorJunkAction::IsJunk(Item* item) const
{
    if (!item)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return false;

    // Nothing to gain, and destroying it would be pure loss.
    if (!proto->SellPrice)
        return false;

    // Never touch anything the bot is wearing or that is locked up in a trade/auction flow.
    if (item->IsEquipped() || item->IsInTrade())
        return false;

    // Conjured items vanish on logout anyway, and quest items are never junk regardless of quality.
    if (proto->Class == ITEM_CLASS_QUEST)
        return false;

    // Defer to the shared classifier for anything it has an opinion about: it already knows about
    // quest needs, ammo, profession reagents and the master's requirements. Only sell what it
    // independently agrees is vendor fodder.
    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", proto->ItemId);

    // Above the junk threshold, outgrown soulbound gear is the only other thing worth selling, and
    // it needs a different test -- checked before the VENDOR/NONE filter because the classification
    // it acts on, ITEM_USAGE_BAD_EQUIP, would not survive it.
    if (proto->Quality > sPlayerbotAIConfig.autoVendorJunkMaxQuality)
        return IsOutleveledGear(item, proto, usage);

    // At or below the junk threshold, gear the bot will not wear is junk as well.
    //
    // The branch above already knows ITEM_USAGE_BAD_EQUIP cannot survive the VENDOR/NONE filter --
    // its own comment says so -- but it only routes *above*-threshold items around it. Below the
    // threshold they fell straight into the filter and were rejected, so grey **gear** could never
    // be sold while every other kind of grey could. An operator watched a bot with full bags clear
    // its cloth and vendor trash and sit on a Wooden Shield, Worn Mail Pants, a Worn Hatchet and a
    // Chipped Quarterstaff indefinitely.
    //
    // A grey item the classifier does not want equipped has no other use: it cannot be listed,
    // nobody wants it, and its vendor price is the whole of its value. That is what the quality
    // threshold means. EQUIP and REPLACE are deliberately still excluded, because a low enough bot
    // really may want to wear a grey.
    if (usage == ITEM_USAGE_BAD_EQUIP || usage == ITEM_USAGE_BROKEN_EQUIP)
        return true;

    if (usage != ITEM_USAGE_VENDOR && usage != ITEM_USAGE_NONE)
    {
        // Says which verdict kept a worthless item in the bags. Only fires for items at or below the
        // junk threshold that were rejected anyway, which is rare and is exactly the case worth
        // seeing: 638 grey weapons and armour sat across 160 bots and no count of what *was* sold
        // could show why.
        LOG_DEBUG("playerbots", "[JunkCheck] {} kept grey {} (class {}) -- usage {}", bot->GetName(),
                  proto->Name1, uint32(proto->Class), uint32(usage));
        return false;
    }

    return true;
}

bool VendorJunkAction::IsOutleveledGear(Item* item, ItemTemplate const* proto, ItemUsage usage) const
{
    // ITEM_USAGE_BAD_EQUIP is the verdict that matters here: it means the bot weighed this piece
    // against what it is wearing and decided against it. ITEM_USAGE_VENDOR counts too, for gear it
    // cannot use at all. ITEM_USAGE_NONE deliberately does not -- "no opinion" is a fine reason to
    // sell a grey and nowhere near enough to justify destroying a rare.
    if (usage != ITEM_USAGE_BAD_EQUIP && usage != ITEM_USAGE_VENDOR)
        return false;

    // Gear only. Restricting the class keeps this away from containers, reagents and consumables --
    // a spare epic bag and a stack of rare crafting mats both reach the vendor branch for perfectly
    // ordinary reasons, and neither is something to sell off behind the operator's back.
    if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
        return false;

    if (proto->Quality > sPlayerbotAIConfig.vendorOutleveledGearMaxQuality)
        return false;

    // The load-bearing guard. QueryItemUsageForEquip returns ITEM_USAGE_NONE both for gear the bot
    // has outgrown and for gear it has not grown into yet -- BotCanUseItem fails the level check and
    // the reason is gone by the time we see the answer. Without this, a level 13 character would
    // vendor the level 20 blue it was saving, which is the one outcome worse than a full bag.
    if (proto->RequiredLevel > bot->GetLevel())
        return false;

    // Soulbound is what makes this the last resort rather than the first. Anything still tradeable
    // classifies as ITEM_USAGE_AH or ITEM_USAGE_DISENCHANT further up and never reaches here, so it
    // goes to the auction house or the enchanter as it should; reaching this point means there is
    // genuinely nothing else to be done with the item.
    if (!item->IsSoulBound())
        return false;

    // The bot must already be wearing something in the slot this would go to.
    //
    // BAD_EQUIP does not only mean "worse than what I have". It is also returned for an empty slot
    // whose candidate scored zero, which is most low level armour -- it carries no stats, so its
    // whole score is a tiebreaker. Selling on the verdict alone would strip bots of the only gear
    // they had for that slot, which is the opposite of the point. An occupied slot is what makes
    // this surplus rather than the bot's only option.
    uint8 const slot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
    if (slot == NULL_SLOT)
        return false;

    return bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) != nullptr;
}

uint32 VendorJunkAction::SellWithoutVendor(Item* item)
{
    // Delegates to BotEconomyMgr so there is exactly one implementation of "convert an item to gold
    // without walking to a vendor" -- the auction give-up path needs the same thing, and two copies
    // would drift on the gold-cheat rule.
    return sBotEconomyMgr.SellToVendor(bot, item, botAI->HasCheat(BotCheatMask::gold));
}

bool VendorJunkAction::Execute(Event /*event*/)
{
    CollectBagItemsVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    uint32 sold = 0;
    uint32 earned = 0;

    for (Item* item : visitor.items)
    {
        if (!IsJunk(item))
            continue;

        // Snapshot the name before the item is destroyed.
        std::string const name = item->GetTemplate()->Name1;
        earned += SellWithoutVendor(item);
        sold++;

        LOG_DEBUG("playerbots", "[Logistics] {} auto-vendored {} ({} total copper this pass)",
                  bot->GetName(), name, earned);
    }

    if (!sold)
        return false;

    LOG_DEBUG("playerbots", "[Logistics] {} auto-vendored {} junk item(s) for {} copper", bot->GetName(), sold,
              earned);
    return true;
}
