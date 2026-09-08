/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTSAFETYMGR_H
#define PLAYERBOTS_BOTSAFETYMGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>

class Player;

/**
 * Catches bots that have fallen out of the world and puts them back.
 *
 * Bots path more loosely than players do and end up inside geometry that was never meant to hold
 * anything. From there one of two things happens, both of which the operator reported: the bot
 * accelerates away as though flying, or it strikes a boundary and dies, costing a corpse run and
 * thirty seconds at a spirit healer.
 *
 * Three layers were planned, cheapest first. This is layers one and two: detect and restore, and
 * never die for it. Narrowing the movement generator's slope handling -- the actual cause -- is
 * layer three and deliberately does not block these, because recovery works whatever the cause
 * turns out to be.
 *
 * Falling out of the world is a bug, not a death. A bot must never be sent to a spirit healer for
 * something the server did to it.
 */
class BotSafetyMgr
{
public:
    static BotSafetyMgr& instance()
    {
        static BotSafetyMgr instance;
        return instance;
    }

    /// Per-bot tick. Records safe ground and recovers from falls. Called from the bot update hook.
    void Update(Player* bot, uint32 diff);

    /// Forget a bot's recorded position, e.g. on logout.
    void Forget(ObjectGuid guid);

    std::string DescribeStats() const;

private:
    BotSafetyMgr() = default;
    ~BotSafetyMgr() = default;

    BotSafetyMgr(BotSafetyMgr const&) = delete;
    BotSafetyMgr& operator=(BotSafetyMgr const&) = delete;

    struct Anchor
    {
        uint32 mapId{0};
        Position pos;
        uint32 timer{0};
        bool valid{false};
    };

    /// True if the bot is below the map's floor -- the core's own out-of-world test.
    static bool IsOutOfWorld(Player* bot);

    /// True if the bot is standing on something solid, and so worth remembering.
    static bool IsOnSafeGround(Player* bot);

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, Anchor> _anchors;

    uint32 _recoveries{0};
    uint32 _recoveriesNoAnchor{0};
};

#define sBotSafetyMgr BotSafetyMgr::instance()

#endif
