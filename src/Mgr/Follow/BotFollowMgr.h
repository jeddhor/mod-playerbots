/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTFOLLOWMGR_H
#define PLAYERBOTS_BOTFOLLOWMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>

class Player;

/**
 * Keeps grouped bots with the player they are following, across the three ways a player leaves.
 *
 * A bot that follows on foot loses its master the moment the master stops walking: a GM teleport, a
 * mage portal and a warlock summon all move the master somewhere the bot cannot walk to, usually
 * onto another map entirely. Today the group survives and the bots do not arrive, and the only
 * remedy is a chat command typed per bot.
 *
 * Two mechanisms cover all three cases. A summon is offered to the bot directly, so it is accepted;
 * a portal or a GM teleport is not offered at all, so the bot notices its master is on another map
 * and follows.
 */
class BotFollowMgr
{
public:
    static BotFollowMgr& instance()
    {
        static BotFollowMgr instance;
        return instance;
    }

    /// Per-bot tick.
    void Update(Player* bot, uint32 diff);

    std::string DescribeStats() const;

private:
    BotFollowMgr() = default;
    ~BotFollowMgr() = default;

    BotFollowMgr(BotFollowMgr const&) = delete;
    BotFollowMgr& operator=(BotFollowMgr const&) = delete;

    /// Accept a summon that is already waiting for this bot. True if one was taken.
    bool AcceptPendingSummon(Player* bot, Player* master);

    /// Follow a master who is no longer on this bot's map. True if the bot was moved.
    bool FollowAcrossMaps(Player* bot, Player* master);

    /// The human this bot is following, or null if it is not following anyone.
    static Player* FollowedMaster(Player* bot);

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, uint32> _timers;

    uint32 _summonsAccepted{0};
    uint32 _followedAcrossMaps{0};
    uint32 _refused{0};
};

#define sBotFollowMgr BotFollowMgr::instance()

#endif
