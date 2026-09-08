/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTREPAIRMGR_H
#define PLAYERBOTS_BOTREPAIRMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>

class Player;

/**
 * Keeps bots' gear serviceable, with different rules per kind of bot.
 *
 * Durability exists to tax the player's attention: go to a vendor, spend gold, or fight at reduced
 * effectiveness. That tax is only interesting for a character someone is actually playing.
 *
 *   random bots  - repaired free. They are scenery for the realm's economy and questing; making
 *                  them shop for repairs buys nothing and costs travel time.
 *   alt bots     - repaired free, for the same reason. An alt exists to do something useful while
 *                  its owner is elsewhere, not to be driven to a blacksmith.
 *   self bots    - pay. This is the player's own character playing itself, so the cost stays real.
 *                  Repairs happen wherever the bot stands rather than at a vendor: the point is
 *                  that the gold is spent, not that the walk was made.
 *
 * Repair outranks every other purchase in the spending budget already (NeedMoneyFor::repair sits
 * above spells), so a self bot saving for a repair will not train the gold away first.
 */
class BotRepairMgr
{
public:
    static BotRepairMgr& instance()
    {
        static BotRepairMgr instance;
        return instance;
    }

    /// Per-bot tick. Cheap on every call but the periodic one.
    void Update(Player* bot, uint32 diff);

    /// Fraction of equipped durability remaining, 0.0-1.0. Returns 1.0 when nothing can be damaged.
    static float DurabilityFraction(Player* bot);

    /// True when this bot pays for its own repairs, i.e. it is a self bot.
    static bool PaysForRepairs(Player* bot);

    std::string DescribeStats() const;

private:
    BotRepairMgr() = default;
    ~BotRepairMgr() = default;

    BotRepairMgr(BotRepairMgr const&) = delete;
    BotRepairMgr& operator=(BotRepairMgr const&) = delete;

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, uint32> _timers;

    uint32 _freeRepairs{0};
    uint32 _paidRepairs{0};
    uint32 _deferredNoGold{0};
    uint64 _goldSpent{0};
};

#define sBotRepairMgr BotRepairMgr::instance()

#endif
