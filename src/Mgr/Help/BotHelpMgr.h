/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTHELPMGR_H
#define PLAYERBOTS_BOTHELPMGR_H

#include "Define.h"
#include "ObjectGuid.h"

#include <shared_mutex>
#include <string>
#include <vector>

class Player;

/**
 * Bots that are stuck on a quest, and the bots coming to help.
 *
 * Grouping already existed and was switched on in P7.8, but it finds partners by proximity, and the
 * measurement was unambiguous: bots see *nobody*. `[GroupScan]` reported "saw 0 nearby" for almost
 * every scan. Two hundred bots spread across seventy zones rarely stand within sight of a
 * level-appropriate stranger, and widening the radius does not fix a population problem -- it
 * enlarges an empty circle.
 *
 * So this is not an enhancement on top of proximity grouping; it is the mechanism that makes
 * grouping possible at all. A bot that cannot solo something publishes where it is, and eligible
 * bots *travel there*. Once they arrive they are co-located, and the existing proximity invite
 * forms the group without further help. Need-driven, rather than waiting for coincidence.
 */
class BotHelpMgr
{
public:
    static BotHelpMgr& instance()
    {
        static BotHelpMgr instance;
        return instance;
    }

    struct Request
    {
        ObjectGuid caller;
        uint32 questId{0};
        uint32 questLevel{0};
        uint32 mapId{0};
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        uint8 callerLevel{0};
        uint32 expiresAt{0};
        uint8 responders{0};
    };

    /**
     * A bot has failed this quest objective enough times to admit it needs help.
     *
     * Raised on the second failure, deliberately before QuestBlacklistMgr's threshold: a quest that
     * merely needed a second pair of hands should get one before it is written off as unworkable.
     */
    void RaiseRequest(Player* caller, uint32 questId);

    /**
     * A request this bot could answer, or nullptr.
     *
     * Eligibility is anchored to the **quest's** level, not the caller's. A level 80 within two
     * levels of a level 78 caller is fine; a level 80 answering a level 12 quest trivialises it no
     * matter how close the two are in level. That is the operator's fairness rule, and the caller's
     * level alone cannot express it.
     */
    bool FindRequestFor(Player* responder, Request& out);

    /// Count a responder against a request's cap, so a call for help does not become a mob.
    void AcceptRequest(ObjectGuid caller, ObjectGuid responder);

    /// Drop a request: objective done, caller gone, or expired.
    void ClearRequest(ObjectGuid caller);

    /// Expire stale requests. World thread.
    void Update(uint32 diff);

    std::string DescribeRequests() const;

private:
    BotHelpMgr() = default;
    ~BotHelpMgr() = default;

    BotHelpMgr(BotHelpMgr const&) = delete;
    BotHelpMgr& operator=(BotHelpMgr const&) = delete;

    mutable std::shared_mutex _mutex;
    std::vector<Request> _requests;
    uint32 _timer{0};

    uint32 _raised{0};
    uint32 _answered{0};
};

#define sBotHelpMgr BotHelpMgr::instance()

#endif
