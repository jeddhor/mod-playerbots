/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTLOGISTICSMGR_H
#define PLAYERBOTS_BOTLOGISTICSMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>

class Player;

/**
 * Keeping a bag usable, on a timer rather than on a dice roll.
 *
 * Junk vendoring and auction posting are actions on "random" triggers inside the loot strategy, and
 * a random trigger only gets a turn when the non-combat engine runs and the roll comes up. For a
 * random bot standing about between fights that is often enough. For a self bot being played --
 * fighting, looting, moving, fighting again -- the non-combat engine barely gets a turn at all: a
 * character sitting at 112 of 112 slots for a whole evening had 722 combat pushes in the same window
 * and not one `post auctions` or `vendor junk`. The bags fill, looting stops, and the owner watches
 * a bot that manages nothing.
 *
 * This is the fourth time the same shape of bug has turned up -- training, mail collection and
 * disenchanting were all actions nothing ever triggered -- and the fix is the same: work that must
 * happen for every bot regardless of what it is doing does not belong behind a behaviour selector.
 *
 * It deliberately calls the existing actions through DoSpecificAction rather than reimplementing
 * them. Every rule about what may be sold -- the supervised-bot exception, the two bag thresholds,
 * the classifier's verdict, disenchant fodder, quest items -- stays in one place; only the question
 * of *when* to ask moves here.
 */
class BotLogisticsMgr
{
public:
    static BotLogisticsMgr& instance()
    {
        static BotLogisticsMgr instance;
        return instance;
    }

    /// Per-bot tick. Cheap except on the periodic pass.
    void Update(Player* bot, uint32 diff);

    std::string DescribeStats() const;

private:
    BotLogisticsMgr() = default;
    ~BotLogisticsMgr() = default;

    BotLogisticsMgr(BotLogisticsMgr const&) = delete;
    BotLogisticsMgr& operator=(BotLogisticsMgr const&) = delete;

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, uint32> _timers;

    uint32 _passes{0};
    uint32 _junkPasses{0};
    uint32 _auctionPasses{0};
};

#define sBotLogisticsMgr BotLogisticsMgr::instance()

#endif
