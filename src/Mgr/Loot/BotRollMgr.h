/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTROLLMGR_H
#define PLAYERBOTS_BOTROLLMGR_H

#include "Define.h"
#include "ObjectGuid.h"

#include <mutex>
#include <unordered_map>
#include <vector>

class Player;

/**
 * Holds a bot's loot roll for a moment before casting it.
 *
 * Bots answered rolls from inside the handler for SMSG_LOOT_START_ROLL -- the vote was registered in
 * the same tick the roll began. For an ordinary bot that is invisible. For a **selfbot**, where the
 * bot AI drives a real player's session, it is not: the roll can be decided and torn down server-side
 * before that player's client has finished building the roll frame, so nothing ever arrives to
 * dismiss it. The dialog then sits there permanently, surviving even its own progress bar running
 * out, because by then the roll no longer exists to send anything about.
 *
 * This is why the operator only ever saw it in selfbot mode, and why two human players never did: a
 * person takes a second or two to click.
 *
 * Delaying the vote gives the client time to receive the roll, build the frame, and then receive the
 * resolution that closes it. It also stops bots answering rolls in zero milliseconds, which is a
 * tell no real group has.
 */
class BotRollMgr
{
public:
    static BotRollMgr& instance()
    {
        static BotRollMgr instance;
        return instance;
    }

    /// Queue a vote to be cast shortly. `choice` is a RollVote.
    void QueueRoll(Player* bot, ObjectGuid lootGuid, uint8 choice);

    /// Cast any votes that have come due for this bot.
    void Update(Player* bot, uint32 diff);

    void Forget(ObjectGuid guid);

private:
    BotRollMgr() = default;
    ~BotRollMgr() = default;

    BotRollMgr(BotRollMgr const&) = delete;
    BotRollMgr& operator=(BotRollMgr const&) = delete;

    struct PendingRoll
    {
        ObjectGuid lootGuid;
        uint8 choice{0};
        int32 remainingMs{0};
    };

    std::mutex _mutex;
    std::unordered_map<ObjectGuid, std::vector<PendingRoll>> _pending;
};

#define sBotRollMgr BotRollMgr::instance()

#endif
