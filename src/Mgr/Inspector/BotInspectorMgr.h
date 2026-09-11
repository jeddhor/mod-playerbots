/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTINSPECTORMGR_H
#define PLAYERBOTS_BOTINSPECTORMGR_H

#include "Define.h"
#include "ObjectGuid.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

/**
 * Answers queries from the Bot Inspector addon.
 *
 * A WoW addon cannot read the server. Everything it displays must have been sent to it, so this is
 * the half of that tool that lives server-side; the addon is the smaller half.
 *
 * Everything is on demand and nothing is pushed. The addon asks for the zones that have bots, then
 * for one zone's roster, then for one bot's detail. That shape exists because the naive design --
 * send every bot and filter client-side -- is about 125 chunks for 500 bots at the 255-byte addon
 * message limit, and it would have been re-sent on a timer. Asking per zone keeps every response
 * small and makes the realm's size stop mattering: a 3000-bot server costs no more per interaction
 * than a 200-bot one, because nothing ever asks for "all bots".
 *
 * It is a debugging tool. Nothing here needs to be live, which is what allows the roster diff, the
 * refresh timer and the cache invalidation to be omitted entirely -- and with them the class of bug
 * where the panel confidently shows data from thirty seconds ago.
 */
class BotInspectorMgr
{
public:
    static BotInspectorMgr& instance()
    {
        static BotInspectorMgr instance;
        return instance;
    }

    /**
     * Handle one addon message. Returns true if it was ours and has been dealt with.
     *
     * `msg` is the raw addon payload; the caller has already established it came over an addon
     * channel.
     */
    bool HandleMessage(Player* sender, std::string const& msg);

    /**
     * Deferred work for one player, called from the world update.
     *
     * Only party invites need this. Adding an alt bot starts a login, and a login does not finish
     * inside the request that asked for it -- so the invite cannot be issued there. Queuing it and
     * retrying as the master ticks is what turns "add" and "join my party" into one click.
     */
    void Update(Player* player, uint32 diff);

private:
    BotInspectorMgr() = default;
    ~BotInspectorMgr() = default;

    BotInspectorMgr(BotInspectorMgr const&) = delete;
    BotInspectorMgr& operator=(BotInspectorMgr const&) = delete;

    /// Send one logical response, split across as many addon messages as it needs.
    void Reply(Player* to, std::string const& verb, std::string const& key,
               std::vector<std::string> const& rows);

    void SendError(Player* to, uint32 code, std::string const& text);

    void HandleZones(Player* to);
    void HandleList(Player* to, uint32 zoneId);
    void HandleFind(Player* to, std::string const& needle);
    void HandleDetail(Player* to, ObjectGuid::LowType botGuid, std::string const& section);

    /// The requesting account's other characters, with enough state for the panel to offer actions.
    void HandleAlts(Player* to);

    /// ADD / REMOVE / INVITE for one of the requesting account's own characters.
    void HandleAltControl(Player* to, std::string const& action, ObjectGuid::LowType altGuid);

    /**
     * Live debugging state for the character being played, when it is a self bot.
     *
     * Everything here is already in the server's head and nowhere a person can see it. Watching a
     * bot walk off and having no idea whether it is heading for a gather node, a quest giver or a
     * grind spot -- and no way to tell a long walk apart from a stuck one -- is most of what has
     * made self-bot behaviour hard to reason about. This is that state, cheap enough to poll.
     */
    void HandleSelfStat(Player* to);

    /// Put `bot` in `master`'s group, creating the group if this is the first member. True on success.
    bool JoinMasterParty(Player* master, Player* bot);

    /**
     * True if this account may inspect bots at all.
     *
     * Two gates, and the second matters more than the first: requests are honoured only above a
     * configured GM level, **and** only for targets that are actually bots. Without the second
     * check this becomes a general player-inspection tool, which is a different feature with
     * different consent implications, and not one anybody asked for.
     */
    bool IsAllowed(Player* sender) const;

    /**
     * Per-account token bucket, so a held-down key cannot turn into a realm scan per keystroke.
     *
     * A minimum interval between requests was the obvious thing and it was wrong. Selecting a bot
     * fires four section requests from one Lua loop, which arrive inside the same world tick; a
     * 100ms floor rejected three of them, and because a rejection was silent the panel reported it
     * as "server has no inspector". A bucket admits that burst by design and still throttles a
     * sustained flood, which is the behaviour actually wanted.
     *
     * Cost is per verb: the cheap listings are one token, FIND is three because it is the only
     * request whose cost scales with bot count, and RECIPE is five because it is the only response
     * measured in tens of messages.
     */
    bool RateLimit(Player* sender, std::string const& verb, std::string const& section);

    struct Bucket
    {
        float tokens = 0.0f;
        uint32 lastMs = 0;
    };

    /**
     * A party invite waiting for a freshly added alt bot to reach the world.
     *
     * Held by GUID rather than by pointer: the whole point is that the bot does not exist as a
     * Player yet when this is queued, and it may never arrive if the login fails.
     */
    struct PendingInvite
    {
        ObjectGuid master;
        ObjectGuid bot;
        uint32 remainingMs = 0;
    };

    std::mutex _mutex;
    std::unordered_map<uint32, Bucket> _buckets;
    std::vector<PendingInvite> _pendingInvites;
};

#define sBotInspectorMgr BotInspectorMgr::instance()

#endif
