/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTEVENTLOGMGR_H
#define PLAYERBOTS_BOTEVENTLOGMGR_H

#include "Define.h"
#include "ObjectGuid.h"

#include <deque>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

/**
 * A short history of what a self bot actually did, for the person whose character it is.
 *
 * The inspector's stats panel answers "what is it doing now"; this answers "what has it been
 * doing", which is a different question and the one that matters after something goes wrong. A
 * character that quietly sold an item, spent its gold, or died offscreen leaves no trace otherwise:
 * the combat log is gone by the time anyone looks, and the server log is 500MB of two hundred bots.
 *
 * Only self bots are recorded. A clientless bot has nobody to show this to, and three thousand ring
 * buffers would be a real cost for no reader.
 */
class BotEventLogMgr
{
public:
    static BotEventLogMgr& instance()
    {
        static BotEventLogMgr instance;
        return instance;
    }

    /**
     * One-character event class, so the client can colour and filter without parsing prose.
     *
     * Kept deliberately coarse: the text carries the detail, and a category exists only where the
     * reader would plausibly want to see that kind of line on its own.
     */
    enum class Cat : char
    {
        Sell     = 'S',  ///< vendored, by travel or by the no-travel shortcut
        Buy      = 'B',  ///< bought from a vendor or trainer
        Auction  = 'A',  ///< listed, bought, sold or expired on the house
        Use      = 'U',  ///< consumed an item
        Loot     = 'L',  ///< picked something up worth mentioning
        Quest    = 'Q',  ///< accepted, completed, turned in or abandoned
        Mail     = 'M',  ///< read or collected mail
        Gold     = 'G',  ///< money moved without a more specific event explaining it
        Death    = 'D',  ///< died, and to what
        Note     = 'N',  ///< anything else worth surfacing while debugging
    };

    /**
     * Record one event against `player`.
     *
     * Silently does nothing for a player with no self bot attached, so callers do not each need the
     * IsSelfBot test. `deltaCopper` is the money this event moved, signed, or 0 when it moved none;
     * the running balance is read from the player and stored alongside so the client can show both
     * without having to reconstruct either.
     */
    void Record(Player* player, Cat cat, std::string const& text, int64 deltaCopper = 0);

    /**
     * Events newer than `afterSeq`, oldest first, at most `limit`.
     *
     * The client passes back the highest sequence it has seen, so a poll returns only what is new
     * and a client that missed a poll catches up without a special case. Sequence numbers are per
     * player and never reused within a session.
     */
    std::vector<std::string> GetSince(Player* player, uint32 afterSeq, uint32 limit) const;

    /// Drop a player's history. Called on logout: it is a session view, not a permanent record.
    void Forget(ObjectGuid guid);

private:
    struct Event
    {
        uint32 seq;
        uint32 when;        ///< unix time, so the client can show wall-clock without a clock sync
        Cat cat;
        int64 balance;      ///< copper held after the event
        int64 delta;        ///< copper this event moved, signed
        std::string text;
    };

    struct Log
    {
        std::deque<Event> events;
        uint32 nextSeq{1};
    };

    /// Enough to cover a long session's interesting moments without being a memory decision.
    static constexpr size_t MAX_EVENTS = 400;

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, Log> _logs;
};

#define sBotEventLogMgr BotEventLogMgr::instance()

#endif
