/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotRollMgr.h"

#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"

void BotRollMgr::QueueRoll(Player* bot, ObjectGuid lootGuid, uint8 choice)
{
    if (!bot)
        return;

    PendingRoll roll;
    roll.lootGuid = lootGuid;
    roll.choice = choice;

    // Jittered rather than fixed, so a group of bots does not answer in perfect unison -- which
    // looks stranger than answering instantly.
    roll.remainingMs = static_cast<int32>(urand(sPlayerbotAIConfig.lootRollDelayMinMs,
                                                std::max(sPlayerbotAIConfig.lootRollDelayMinMs,
                                                         sPlayerbotAIConfig.lootRollDelayMaxMs)));

    std::lock_guard<std::mutex> guard(_mutex);
    _pending[bot->GetGUID()].push_back(roll);
}

void BotRollMgr::Update(Player* bot, uint32 diff)
{
    if (!bot)
        return;

    std::vector<PendingRoll> due;

    {
        std::lock_guard<std::mutex> guard(_mutex);

        auto itr = _pending.find(bot->GetGUID());
        if (itr == _pending.end() || itr->second.empty())
            return;

        auto& rolls = itr->second;
        for (auto r = rolls.begin(); r != rolls.end();)
        {
            r->remainingMs -= static_cast<int32>(diff);
            if (r->remainingMs <= 0)
            {
                due.push_back(*r);
                r = rolls.erase(r);
            }
            else
            {
                ++r;
            }
        }

        if (rolls.empty())
            _pending.erase(itr);
    }

    // Cast outside the lock: CountRollVote can resolve the roll, which sends packets and touches
    // group state, and holding a lock across that is asking for trouble.
    for (PendingRoll const& roll : due)
    {
        Group* group = bot->GetGroup();
        if (!group)
            continue;

        // dismissVoterFrame: this vote came from the bot, not from anyone clicking, so the
        // character's own need/greed frame has to be closed explicitly. On a self bot that frame is
        // on the operator's screen.
        group->CountRollVote(bot->GetGUID(), roll.lootGuid, roll.choice, true);

        LOG_DEBUG("playerbots", "[Roll] {} answered a loot roll with choice {}", bot->GetName(), roll.choice);
    }
}

void BotRollMgr::Forget(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(_mutex);
    _pending.erase(guid);
}
