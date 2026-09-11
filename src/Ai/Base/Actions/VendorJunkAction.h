/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_VENDORJUNKACTION_H
#define PLAYERBOTS_VENDORJUNKACTION_H

#include "InventoryAction.h"
#include "ItemUsageValue.h"

class Item;
class PlayerbotAI;
struct ItemTemplate;

/**
 * Sells junk at vendor price without travelling to a vendor.
 *
 * A clogged inventory silently disables almost everything else a bot does: looting stops, quest
 * items cannot be picked up, and gathered materials are lost. Making bots walk to a vendor to fix
 * that is a lot of pathfinding for no gameplay value, so this is one of the sanctioned "bot-like"
 * abstractions - the same class as posting to the auction house without visiting an auctioneer.
 *
 * The gold is real and the item is really destroyed, so the realm's money supply moves exactly as
 * it would have if the bot had walked to a vendor and clicked sell. Only the travel is skipped.
 *
 * Deliberately conservative about what counts as junk: a non-zero sell price, not equipped, not
 * soulbound-but-useful, and never anything a quest needs. Below the configured quality threshold
 * (poor by default) that is the whole test.
 *
 * Above it, one further class of item qualifies: soulbound gear the bot has outgrown. Greens and
 * blues that bind on pickup accumulate with nothing to spend them on -- they cannot be listed,
 * traded or handed to a groupmate, and only an enchanter can break them down -- so a bot that never
 * sells them fills its bags with gear it will never wear again. See IsOutleveledGear.
 */
class VendorJunkAction : public InventoryAction
{
public:
    VendorJunkAction(PlayerbotAI* botAI, std::string const name = "vendor junk")
        : InventoryAction(botAI, name)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    /// True if this item should be auto-sold. Kept separate so it can be unit-reasoned about.
    bool IsJunk(Item* item) const;
    /**
     * True for soulbound gear the bot has outgrown -- green and blue drops that cannot be traded,
     * listed or disenchanted and so have no remaining use but their vendor price.
     *
     * Separate from IsJunk because it answers a different question. IsJunk asks "is this worthless?";
     * this asks "is this worthless *and* is selling it the only thing left to do with it?", which
     * needs the item instance rather than just the template and has to be sure the bot is not simply
     * too low a level to wear it yet.
     */
    bool IsOutleveledGear(Item* item, ItemTemplate const* proto, ItemUsage usage) const;
    /// Credits the bot with the vendor price and destroys the stack. Returns copper gained.
    uint32 SellWithoutVendor(Item* item);
};

#endif
