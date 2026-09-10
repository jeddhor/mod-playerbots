/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTLFGMGR_H
#define PLAYERBOTS_BOTLFGMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

/**
 * Makes the Dungeon Finder answer a person, instead of making them wait for a coincidence.
 *
 * Bots do queue: LfgStrategy fires `lfg join` off the `random` trigger, so every unsupervised bot
 * occasionally puts itself in for dungeons it picks itself. That is fine for bots keeping each
 * other busy and useless to a person, because nothing connects the two. A player who queues waits
 * until enough bots happen to have chosen a compatible dungeon at a compatible level in the same
 * bracket at the same moment. On a realm with a handful of level 80s that is close to never, which
 * is exactly what was observed: the queue simply does not pop.
 *
 * So the queue is made demand-driven. When a person is waiting, this looks at what they actually
 * queued for and puts suitable bots into that same queue, with the roles the group is short of.
 * The bots are not teleported and nothing is faked -- they join the real queue, LFGQueue assembles
 * the group and the proposal goes out as normal, so everything downstream (role checks, the
 * teleport in, the dungeon itself) is the code that already runs.
 *
 * The population limit is real and deliberately not worked around. Seeding can only queue bots that
 * exist at the right level, so a fresh realm of level 1 bots will not fill a level 70 dungeon until
 * bots have grown into it. That is the accepted cost of a world that levels honestly.
 */
class BotLfgMgr
{
public:
    static BotLfgMgr& instance()
    {
        static BotLfgMgr instance;
        return instance;
    }

    /// World-thread tick.
    void Update(uint32 diff);

    std::string DescribeStats() const;

private:
    BotLfgMgr() = default;
    ~BotLfgMgr() = default;

    BotLfgMgr(BotLfgMgr const&) = delete;
    BotLfgMgr& operator=(BotLfgMgr const&) = delete;

    /// One person waiting in the queue, and what they are waiting for.
    struct Waiter
    {
        ObjectGuid guid;
        std::vector<uint32> dungeons;
        uint8 level{0};
        uint8 team{0};
        uint32 partySize{1};
        bool hasTank{false};
        bool hasHealer{false};
    };

    /// A bot this manager put into the queue, so it can be taken back out again.
    struct Seed
    {
        ObjectGuid forPlayer;
        uint32 queuedAtMs{0};
    };

    std::vector<Waiter> CollectWaiters() const;
    void ServeWaiter(Waiter const& waiter);
    void ReleaseStaleSeeds();

    /// Queue one bot for exactly these dungeons. True if the packet was sent.
    bool QueueBot(Player* bot, std::vector<uint32> const& dungeons, ObjectGuid forPlayer);

    /// Take a seeded bot back out of the queue.
    void UnqueueBot(Player* bot);

    /// Dungeons from the player's selection this bot is actually eligible for.
    static std::vector<uint32> EligibleDungeons(Player* bot, Waiter const& waiter);

    static bool IsSeedable(Player* bot);

    mutable std::shared_mutex _mutex;
    /// bot guid -> why it is in the queue.
    std::unordered_map<ObjectGuid, Seed> _seeded;
    uint32 _nextCheckMs = 0;

    uint32 _waitersServed = 0;
    uint32 _botsQueued = 0;
    uint32 _botsReleased = 0;
    uint32 _noCandidates = 0;
};

#define sBotLfgMgr BotLfgMgr::instance()

#endif
