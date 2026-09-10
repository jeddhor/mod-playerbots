/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotLfgMgr.h"

#include "DBCStores.h"
#include "Group.h"
#include "LFGMgr.h"
#include "LfgActions.h"
#include "Map.h"
#include "Opcodes.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "WorldPacket.h"

#include <algorithm>
#include <cstdlib>

using namespace lfg;

namespace
{
    /// A 5-man is what the Dungeon Finder forms; raids go through a different path entirely.
    constexpr uint32 DUNGEON_GROUP_SIZE = 5;
}

void BotLfgMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.lfgSeedForPlayers)
        return;

    uint32 const now = getMSTime();
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        if (now < _nextCheckMs)
            return;
    }
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _nextCheckMs = now + sPlayerbotAIConfig.lfgSeedIntervalMs;
    }

    ReleaseStaleSeeds();

    std::vector<Waiter> const waiters = CollectWaiters();
    for (Waiter const& waiter : waiters)
        ServeWaiter(waiter);

    (void)diff;
}

std::vector<BotLfgMgr::Waiter> BotLfgMgr::CollectWaiters() const
{
    std::vector<Waiter> waiters;

    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        Player* player = entry.second;
        if (!player || !player->IsInWorld())
            continue;

        // Random bots queue on their own schedule and are the supply, not the demand. Seeding for
        // them would have bots summoning bots in a loop that never settles.
        if (sRandomPlayerbotMgr.IsRandomBot(player))
            continue;

        LfgState const state = sLFGMgr->GetState(player->GetGUID());
        if (state != LFG_STATE_QUEUED && state != LFG_STATE_ROLECHECK)
            continue;

        LfgDungeonSet const& selected = sLFGMgr->GetSelectedDungeons(player->GetGUID());
        if (selected.empty())
            continue;

        Waiter waiter;
        waiter.guid = player->GetGUID();
        waiter.dungeons.assign(selected.begin(), selected.end());
        // Sorted so that two ticks in a row consider the same dungeons in the same order, and the
        // bots picked for a waiter do not churn between ticks.
        std::sort(waiter.dungeons.begin(), waiter.dungeons.end());
        waiter.level = player->GetLevel();
        waiter.team = static_cast<uint8>(player->GetTeamId());

        uint8 const roles = sLFGMgr->GetRoles(player->GetGUID());
        waiter.hasTank = (roles & PLAYER_ROLE_TANK) != 0;
        waiter.hasHealer = (roles & PLAYER_ROLE_HEALER) != 0;

        // A person queueing as a party brings their party with them; only the rest is missing.
        if (Group* group = player->GetGroup())
        {
            waiter.partySize = group->GetMembersCount();

            // Only the leader of a queued party represents it, or a five-person party would be
            // served five times over.
            if (group->GetLeaderGUID() != player->GetGUID())
                continue;

            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player)
                    continue;

                uint8 const memberRoles = sLFGMgr->GetRoles(member->GetGUID());
                waiter.hasTank = waiter.hasTank || (memberRoles & PLAYER_ROLE_TANK) != 0;
                waiter.hasHealer = waiter.hasHealer || (memberRoles & PLAYER_ROLE_HEALER) != 0;
            }
        }

        waiters.push_back(std::move(waiter));
    }

    return waiters;
}

void BotLfgMgr::ServeWaiter(Waiter const& waiter)
{
    uint32 alreadySeeded = 0;
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        for (auto const& entry : _seeded)
            if (entry.second.forPlayer == waiter.guid)
                ++alreadySeeded;
    }

    if (waiter.partySize + alreadySeeded >= DUNGEON_GROUP_SIZE)
        return;

    uint32 wanted = DUNGEON_GROUP_SIZE - waiter.partySize - alreadySeeded;

    // Two passes. The first only accepts bots that can cover a role the group has nobody for,
    // because a queue holding four damage dealers and the person who wanted a tank is the failure
    // this whole manager exists to prevent -- and it is the failure P13.3 already measured, where
    // compatible sets reached four and stopped 539 times.
    bool needTank = !waiter.hasTank;
    bool needHealer = !waiter.hasHealer;

    std::vector<Player*> candidates;
    for (auto const& entry : sRandomPlayerbotMgr.GetAllBotsRef())
    {
        Player* bot = entry.second;
        if (!IsSeedable(bot))
            continue;

        if (static_cast<uint8>(bot->GetTeamId()) != waiter.team)
            continue;

        if (EligibleDungeons(bot, waiter).empty())
            continue;

        candidates.push_back(bot);
    }

    if (candidates.empty())
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        ++_noCandidates;
        return;
    }

    // Closest in level first. A level 80 filling a level 20 person's dungeon is technically legal
    // in a random queue and makes for a worthless run.
    std::sort(candidates.begin(), candidates.end(),
              [&waiter](Player* a, Player* b)
              {
                  return std::abs(int32(a->GetLevel()) - int32(waiter.level)) <
                         std::abs(int32(b->GetLevel()) - int32(waiter.level));
              });

    uint32 queued = 0;
    for (bool rolesFirst : {true, false})
    {
        for (Player* bot : candidates)
        {
            if (queued >= wanted)
                break;

            uint32 const roles = LfgRolesFor(bot);
            bool const canTank = (roles & PLAYER_ROLE_TANK) != 0;
            bool const canHeal = (roles & PLAYER_ROLE_HEALER) != 0;

            if (rolesFirst)
            {
                bool const fillsGap = (needTank && canTank) || (needHealer && canHeal);
                if (!fillsGap)
                    continue;
            }

            std::vector<uint32> const dungeons = EligibleDungeons(bot, waiter);
            if (dungeons.empty())
                continue;

            if (!QueueBot(bot, dungeons, waiter.guid))
                continue;

            ++queued;
            if (canTank)
                needTank = false;
            if (canHeal)
                needHealer = false;
        }

        if (queued >= wanted)
            break;
    }

    if (queued)
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        ++_waitersServed;
        _botsQueued += queued;
    }
}

bool BotLfgMgr::IsSeedable(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (!GET_PLAYERBOT_AI(bot))
        return false;

    // Already busy: in a party, already queued, in a battleground, or standing in an instance.
    // Pulling any of those into a new queue would break something a person is already doing.
    if (bot->GetGroup() || bot->InBattleground())
        return false;

    if (sLFGMgr->GetState(bot->GetGUID()) != LFG_STATE_NONE)
        return false;

    if (bot->IsInCombat())
        return false;

    if (Map* map = bot->FindMap(); map && map->Instanceable())
        return false;

    return true;
}

std::vector<uint32> BotLfgMgr::EligibleDungeons(Player* bot, Waiter const& waiter)
{
    std::vector<uint32> out;

    for (uint32 const dungeonId : waiter.dungeons)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(dungeonId);
        if (!dungeon)
            continue;

        uint8 const level = bot->GetLevel();
        if (dungeon->MinLevel && (level < dungeon->MinLevel || level > dungeon->MaxLevel))
            continue;

        // Expansion the bot's account can actually reach. Queueing a bot for content its client
        // flags exclude produces a proposal that cannot complete.
        if (dungeon->ExpansionLevel > bot->GetSession()->Expansion())
            continue;

        out.push_back(dungeonId);
    }

    return out;
}

bool BotLfgMgr::QueueBot(Player* bot, std::vector<uint32> const& dungeons, ObjectGuid forPlayer)
{
    if (dungeons.empty())
        return false;

    uint32 const roles = LfgRolesFor(bot);

    // Same route LfgJoinAction uses: JoinLfg is not thread safe, so the join goes through the bot's
    // own session as a packet rather than being called here on the world thread.
    WorldPacket* data = new WorldPacket(CMSG_LFG_JOIN);
    *data << (uint32)roles;
    *data << (bool)false;
    *data << (bool)false;
    *data << (uint8)(dungeons.size());
    for (uint32 const dungeonId : dungeons)
        *data << (uint32)dungeonId;
    *data << (uint8)3 << (uint8)0 << (uint8)0 << (uint8)0;
    *data << std::to_string(GET_PLAYERBOT_AI(bot)->GetEquipGearScore(bot));
    bot->GetSession()->QueuePacket(data);

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _seeded[bot->GetGUID()] = Seed{forPlayer, getMSTime()};
    }

    LOG_INFO("playerbots", "[LFG] seeding {} (level {}) into the queue for {}, {} dungeon(s)",
             bot->GetName(), bot->GetLevel(), forPlayer.ToString(), dungeons.size());
    return true;
}

void BotLfgMgr::UnqueueBot(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return;

    // Past LFG_STATE_QUEUED the bot has a proposal or is already inside; leaving then would strand
    // the group it was about to complete.
    if (sLFGMgr->GetState(bot->GetGUID()) > LFG_STATE_QUEUED)
        return;

    WorldPacket* packet = new WorldPacket(CMSG_LFG_LEAVE);
    bot->GetSession()->QueuePacket(packet);
}

void BotLfgMgr::ReleaseStaleSeeds()
{
    uint32 const now = getMSTime();
    uint32 const holdMs = sPlayerbotAIConfig.lfgSeedHoldMs;

    std::vector<ObjectGuid> release;
    std::vector<ObjectGuid> forget;

    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        for (auto const& entry : _seeded)
        {
            Player* bot = ObjectAccessor::FindPlayer(entry.first);
            if (!bot || !bot->IsInWorld())
            {
                forget.push_back(entry.first);
                continue;
            }

            // The bot got into a dungeon: it is no longer a seed, it is a participant.
            if (sLFGMgr->GetState(entry.first) > LFG_STATE_QUEUED)
            {
                forget.push_back(entry.first);
                continue;
            }

            Player* waiter = ObjectAccessor::FindPlayer(entry.second.forPlayer);
            bool const waiterGone = !waiter || !waiter->IsInWorld() ||
                                    sLFGMgr->GetState(entry.second.forPlayer) == LFG_STATE_NONE;

            // Released either because the person we were queueing for gave up, or because the queue
            // never formed. A bot left parked in a queue forever cannot be seeded for the next
            // person, and would slowly drain the pool this depends on.
            if (waiterGone || getMSTimeDiff(entry.second.queuedAtMs, now) > holdMs)
                release.push_back(entry.first);
        }
    }

    for (ObjectGuid const& guid : release)
    {
        if (Player* bot = ObjectAccessor::FindPlayer(guid))
            UnqueueBot(bot);
    }

    if (release.empty() && forget.empty())
        return;

    std::unique_lock<std::shared_mutex> lock(_mutex);
    for (ObjectGuid const& guid : release)
    {
        _seeded.erase(guid);
        ++_botsReleased;
    }
    for (ObjectGuid const& guid : forget)
        _seeded.erase(guid);
}

std::string BotLfgMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);

    std::ostringstream out;
    out << "LFG seeding: " << _seeded.size() << " bots queued for players now; "
        << _waitersServed << " waits served, " << _botsQueued << " bots queued, "
        << _botsReleased << " released, " << _noCandidates << " times no candidate was eligible";
    return out.str();
}
