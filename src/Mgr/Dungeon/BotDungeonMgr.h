/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTDUNGEONMGR_H
#define PLAYERBOTS_BOTDUNGEONMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>

class Player;

/**
 * Lets a bot group clear a dungeon when a human is in it.
 *
 * P13.4 already taught bots to pick pulls inside an instance, but it lives in the `new rpg`
 * strategy, and AiFactory only grants that strategy to bots it considers unsupervised -- bots with
 * no human master. The moment a person invites bots to their own party, AcceptInvitationAction sets
 * that person as master and switches the bots to `+follow`, so the navigation is gone. The group
 * fights whatever it is standing next to and never walks anywhere, which is exactly the reported
 * behaviour: the AI takes over once something is attacking, and a person still has to drive.
 *
 * Handing party lead to a bot is the natural way to say "you drive" -- it is what the operator
 * reached for before reading any of this code -- but leadership had no effect, because the gate was
 * on master and not on who leads. So that is the switch:
 *
 *     inside an instance, if the group leader is a bot, the group runs itself.
 *
 * The leader gets navigation and stops following anyone. Everyone else follows the leader rather
 * than their master, because a follower still glued to a human standing at the entrance is a
 * follower that never reaches the pull.
 *
 * Deliberately limited to instances. In the open world a human who hands lead to an alt for loot
 * reasons should not find that alt walking off to quest, and the follow-the-human behaviour out
 * there already works.
 */
class BotDungeonMgr
{
public:
    static BotDungeonMgr& instance()
    {
        static BotDungeonMgr instance;
        return instance;
    }

    /// Per-bot tick.
    void Update(Player* bot, uint32 diff);

    /// Drop remembered state for a bot that is going away.
    void Forget(ObjectGuid guid);

    std::string DescribeStats() const;

private:
    BotDungeonMgr() = default;
    ~BotDungeonMgr() = default;

    BotDungeonMgr(BotDungeonMgr const&) = delete;
    BotDungeonMgr& operator=(BotDungeonMgr const&) = delete;

    /// What role this bot should be playing right now.
    enum class Mode : uint8
    {
        Off,        ///< Not in an instance, or a human is leading. Normal behaviour.
        Leading,    ///< This bot is the group leader and navigates.
        Following,  ///< A bot leads; this one follows it.
    };

    struct State
    {
        Mode mode{Mode::Off};
        uint32 sinceMs{0};
        /// Formation to put back when autopilot ends, so a player's `.formation` choice survives.
        std::string savedFormation;
    };

    static Mode DecideMode(Player* bot);
    void Apply(Player* bot, Mode mode, State& state);

    void SetFormation(Player* bot, std::string const& name);
    std::string CurrentFormation(Player* bot) const;

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, State> _state;
    std::unordered_map<ObjectGuid, uint32> _nextCheckMs;

    uint32 _engaged = 0;
    uint32 _disengaged = 0;
    uint32 _leaders = 0;
    uint32 _followers = 0;
};

#define sBotDungeonMgr BotDungeonMgr::instance()

#endif
