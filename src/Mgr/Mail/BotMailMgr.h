/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTMAILMGR_H
#define PLAYERBOTS_BOTMAILMGR_H

#include "Define.h"
#include "ObjectGuid.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>

class Player;

/**
 * Empties a bot's mailbox, on a timer rather than as an activity.
 *
 * This used to be an RPG activity (RPG_MAILBOX), chosen from the weighted roll a bot makes when it
 * goes idle. That was the wrong shape for it. Collecting mail needs no travel and takes no time --
 * NewRpgMailboxAction empties the mailbox in place -- so it does not compete with grinding or
 * questing for a bot's attention, and treating it as though it did meant it almost never ran.
 *
 * The measurement that settled it: bots holding auction proceeds had made *one or zero* activity
 * decisions across an entire run, because they spend their time inside long-running states. Raising
 * the activity weight from 5 to 90 changed nothing, because the roll itself was the bottleneck.
 * Meanwhile the auction house delivered mail faster than it was being collected.
 *
 * Same reasoning that moved bot training off the strategy engine: work that must happen for every
 * bot regardless of what it is doing does not belong behind a behaviour selector.
 */
class BotMailMgr
{
public:
    static BotMailMgr& instance()
    {
        static BotMailMgr instance;
        return instance;
    }

    /// Take everything takeable from this bot's mail. True if anything was collected.
    static bool Collect(Player* bot);

    /// Periodic entry point; collects at most once per configured interval.
    void Update(Player* bot, uint32 diff);

    /// One line for .rndbot mail, so an operator can see whether collection is actually running.
    std::string DescribeStats() const;

private:
    BotMailMgr() = default;
    ~BotMailMgr() = default;

    BotMailMgr(BotMailMgr const&) = delete;
    BotMailMgr& operator=(BotMailMgr const&) = delete;

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, uint32> _timers;

    uint32 _collections = 0;
    uint32 _itemsCollected = 0;
    uint64 _moneyCollected = 0;
    uint32 _mailsCleared = 0;
};

#define sBotMailMgr BotMailMgr::instance()

#endif
