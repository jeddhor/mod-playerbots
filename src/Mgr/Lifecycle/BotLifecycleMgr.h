/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTLIFECYCLEMGR_H
#define PLAYERBOTS_BOTLIFECYCLEMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <shared_mutex>
#include <string>

/**
 * P12.2/P12.3 -- a bot's second ending: retirement.
 *
 * Recycling, which the module already does, resets a max-level bot to level 1 and keeps its name,
 * race and class. That models a player rerolling an alt, and it is the right common case -- but it
 * never refreshes the population's *variety*. After a year of it the realm still has exactly the
 * same five hundred names and classes it started with.
 *
 * Retirement is the other ending: the character is deleted and a fresh one takes its place on the
 * same account, with a new name, race and class. A player quit; another signed up.
 *
 * It is deliberately a slow trickle beside recycling (P12.3). Identity is most of what makes a realm
 * read as inhabited, and a population that churns its names every few hours has none.
 *
 * ## Why this could not be built before the economy
 *
 * A retiring bot is not just a character row. It owns auction listings -- some with live bids from
 * other bots -- gold, mail carrying auction proceeds, and a guild membership. `Player::DeleteFromDB`
 * handles the guild, the arena teams, the mail and the items; it does **not** know about auctions.
 * Deleting a seller naively leaves its listings in `sAuctionMgr` pointing at an owner who no longer
 * exists, and destroys the bid another bot has already paid. So retirement settles first:
 *
 *   1. refund every live bid on the retiree's listings, through the core's own cancelled-to-bidder mail
 *   2. destroy the listed items and their auction rows
 *   3. delete the character, which drains its mail and possessions
 *   4. clear its rows from the playerbots tables
 *   5. create a replacement on the same account
 */
class BotLifecycleMgr
{
public:
    static BotLifecycleMgr& instance()
    {
        static BotLifecycleMgr instance;
        return instance;
    }

    /// World thread only: touches AuctionHouseObject and creates characters.
    void Update(uint32 diff);

    std::string DescribeStats() const;

private:
    BotLifecycleMgr() = default;
    ~BotLifecycleMgr() = default;

    BotLifecycleMgr(BotLifecycleMgr const&) = delete;
    BotLifecycleMgr& operator=(BotLifecycleMgr const&) = delete;

    /// One retirement: settle, delete, replace. False if the candidate could not be retired.
    bool Retire(ObjectGuid::LowType guid, uint32 accountId, std::string const& name, uint8 level, uint32 played);

    /// Cancel the retiree's listings, refunding any live bids. Returns listings cancelled.
    uint32 SettleAuctions(ObjectGuid guid);

    /// Drop playerbots_random_bots rows whose character no longer exists.
    void SweepOrphanRows();

    /// Remove the retiree's rows from the module's own tables.
    void ForgetBot(ObjectGuid::LowType guid);

    /// Create a replacement character on the same account. False if none could be made.
    bool CreateReplacement(uint32 accountId, std::string& createdName);

    uint32 _timer{0};

    mutable std::shared_mutex _mutex;
    uint32 _retired{0};
    uint32 _auctionsCancelled{0};
    uint32 _bidsRefunded{0};
    uint32 _replacementsCreated{0};
};

#define sBotLifecycleMgr BotLifecycleMgr::instance()

#endif
