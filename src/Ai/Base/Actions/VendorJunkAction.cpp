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

    if (proto->Quality > sPlayerbotAIConfig.autoVendorJunkMaxQuality)
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
    if (usage != ITEM_USAGE_VENDOR && usage != ITEM_USAGE_NONE)
        return false;

    return true;
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
