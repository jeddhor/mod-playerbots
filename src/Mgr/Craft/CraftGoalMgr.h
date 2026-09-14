/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CRAFTGOALMGR_H
#define PLAYERBOTS_CRAFTGOALMGR_H

#include "ObjectGuid.h"

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class Player;

/**
 * P10.5 -- "I know how to make this, I am short of materials, I will go and get them."
 *
 * Effectively a bot-only repeatable quest, and the thing that turns a profession from a number on a
 * character sheet into somewhere the bot goes and something it does when it gets there.
 *
 * The goal is persisted because it is a multi-trip errand rather than an activity: a bot will cross
 * a zone for it, fight for a while, log out and come back. A goal that did not survive that would
 * restart from nothing each time, and the bot would never finish anything.
 *
 * Abandonment matters as much as selection. A goal expires, and a recipe that repeatedly fails to
 * complete is remembered realm-wide and stops being chosen -- the same lesson as the quest
 * blacklist, which exists because bots otherwise rediscover the same dead end forever.
 */
class CraftGoalMgr
{
public:
    struct Goal
    {
        uint32 spellId{0};
        uint32 itemId{0};
        uint32 createdAt{0};
        uint32 expiresAt{0};

        // Whether the bot has ever reached a farming spot for this goal. In memory only: a goal that
        // expires without being worked says nothing about the recipe, so it must not count against
        // it -- and after a restart the safe assumption is "not worked".
        bool worked{false};
    };

    /// What the bot needs next, and where to get it.
    struct Need
    {
        uint32 itemId{0};
        uint32 missing{0};
        bool farmable{false};
    };

    static CraftGoalMgr& instance()
    {
        static CraftGoalMgr instance;
        return instance;
    }

    void Load();

    /// The bot's current goal, if any. Expired goals are dropped on read. Returned by value: bots on
    /// different maps update on different threads, and a pointer into the map would not survive
    /// another bot's insert rehashing it.
    std::optional<Goal> Current(Player* bot);

    /**
     * The bot's current goal, or a recipe worth farming for if it has none.
     *
     * Prefers recipes that would still grant a skill-up, because a craft that teaches nothing is a
     * trade of materials for an item the bot could usually have bought. Recipes whose missing
     * reagents this bot cannot farm are skipped: buying is Phase 5's job, and a goal that cannot be
     * worked is just a slot occupied.
     *
     * With `commit` false nothing is stored. The availability check asks that way, because offering
     * the activity is not the bot taking it -- a goal stored on every offer would expire unworked
     * whenever the roll went elsewhere.
     */
    std::optional<Goal> Choose(Player* bot, bool commit);

    /// Record that the bot reached a farming spot for its goal, so an expiry now counts as a failure.
    void MarkWorked(Player* bot);

    /// The first reagent the bot is still short of, or an empty Need when everything is present.
    Need NextNeed(Player* bot, Goal const& goal) const;

    void Complete(Player* bot, Goal const& goal);
    void Abandon(Player* bot, Goal const& goal, char const* why);

    std::string DescribeStats() const;

private:
    CraftGoalMgr() = default;
    ~CraftGoalMgr() = default;

    CraftGoalMgr(CraftGoalMgr const&) = delete;
    CraftGoalMgr& operator=(CraftGoalMgr const&) = delete;

    void Store(ObjectGuid::LowType guid, Goal const& goal);
    void Forget(ObjectGuid::LowType guid);

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid::LowType, Goal> _goals;
    std::unordered_map<uint32, uint32> _failures;  //< spell -> times abandoned, realm-wide

    uint32 _chosen{0};
    uint32 _completed{0};
    uint32 _abandoned{0};
};

#define sCraftGoalMgr CraftGoalMgr::instance()

#endif
