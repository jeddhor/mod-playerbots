/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BUYAUCTIONACTION_H
#define PLAYERBOTS_BUYAUCTIONACTION_H

#include "InventoryAction.h"

class PlayerbotAI;

/**
 * Buys gear, reagents and consumables off the auction house.
 *
 * This is the half of the economy that makes the other half mean anything. Posting alone is a
 * one-way valve: listings accumulate, nothing clears, no seller is ever paid, and the price index
 * has no real signal to learn from because nothing ever transacts. A post-only auction house is
 * worse than none, because it looks alive while being dead.
 *
 * What counts as worth buying is delegated to ItemUsageValue, the same classifier the looting and
 * vendoring code uses, so a bot's idea of "an upgrade" is consistent no matter where the item comes
 * from.
 */
class BuyAuctionAction : public InventoryAction
{
public:
    BuyAuctionAction(PlayerbotAI* botAI, std::string const name = "buy auctions")
        : InventoryAction(botAI, name)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
