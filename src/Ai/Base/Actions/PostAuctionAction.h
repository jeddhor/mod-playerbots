/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_POSTAUCTIONACTION_H
#define PLAYERBOTS_POSTAUCTIONACTION_H

#include "InventoryAction.h"

class Item;
class PlayerbotAI;

/**
 * Lists the bot's saleable goods on the auction house.
 *
 * No travel to an auctioneer and no deposit — the same sanctioned abstraction as VendorJunkAction.
 * Everything else is real: the item genuinely leaves the bot's bags, the auction is visible to any
 * player browsing the house, and the seller is paid by the core's own mail when it sells.
 *
 * The decision of *what* is worth listing lives in BotEconomyMgr::ShouldPost, not here, because the
 * buying side needs the same judgement.
 *
 * The posting itself is queued onto the world thread rather than done inline: this action runs
 * during map update, and AuctionHouseObject::AddAuction feeds the multi-threaded
 * AuctionHouseSearcher. The core only ever adds auctions from packet handlers, which are world
 * thread. Doing it inline from a map thread is a data race that would surface as corrupted search
 * results or a crash under load.
 */
class PostAuctionAction : public InventoryAction
{
public:
    PostAuctionAction(PlayerbotAI* botAI, std::string const name = "post auctions")
        : InventoryAction(botAI, name)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
