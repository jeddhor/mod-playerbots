/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotFollowMgr.h"

#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringFormat.h"

namespace
{
/// Often enough that a portal feels followed rather than waited for.
constexpr uint32 FOLLOW_INTERVAL_MS = 2 * 1000;
}  // namespace

Player* BotFollowMgr::FollowedMaster(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;

    Player* master = botAI->GetMaster();

    // A self bot is its own master and has nobody to follow; an alt or random bot following a human
    // does. The bot must also actually be set to follow -- a bot told to go and do its own thing is
    // not lost just because its owner took a portal.
    if (!master || master == bot || !master->IsInWorld())
        return nullptr;

    if (!botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
        return nullptr;

    return master;
}

bool BotFollowMgr::AcceptPendingSummon(Player* bot, Player* master)
{
    // A warlock summon, a meeting stone, or a GM summon all land here: the offer is already sitting
    // on the bot waiting for an answer that never comes, because nothing in the module answers it.
    if (bot->GetSummonExpireTimer() <= GameTime::GetGameTime().count())
        return false;

    if (bot->IsInCombat() || bot->isDead())
        return false;

    bot->SummonIfPossible(true, master->GetGUID());

    std::unique_lock<std::shared_mutex> guard(_mutex);
    ++_summonsAccepted;

    LOG_DEBUG("playerbots", "[Follow] {} accepted a pending summon", bot->GetName());
    return true;
}

bool BotFollowMgr::FollowAcrossMaps(Player* bot, Player* master)
{
    if (bot->GetMapId() == master->GetMapId())
        return false;

    // Not mid-anything. A bot in combat or already being moved is not lost, it is busy.
    if (bot->IsInCombat() || bot->isDead() || bot->IsBeingTeleported() || bot->IsInFlight())
        return false;

    if (master->IsBeingTeleported() || master->IsInFlight())
        return false;

    // Only for a bot actually grouped with its master. Following someone across a map boundary on
    // the strength of a follow strategy alone would drag bots into instances they were never
    // invited to.
    Group* group = bot->GetGroup();
    if (!group || !group->IsMember(master->GetGUID()))
        return false;

    // A battleground or arena is entered by queue, never by following.
    if (master->InBattleground() || master->InArena() || bot->InBattleground() || bot->InArena())
        return false;

    Map* map = master->FindMap();
    if (!map)
        return false;

    // The core decides whether this bot may be on that map at all -- instance locks, level
    // requirements, raid size. Asking it is better than guessing at the rules here.
    if (map->CannotEnter(bot, false) != Map::CAN_ENTER)
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_refused;

        LOG_DEBUG("playerbots", "[Follow] {} cannot enter map {} to follow {}", bot->GetName(), master->GetMapId(),
                  master->GetName());
        return false;
    }

    bot->TeleportTo(master->GetMapId(), master->GetPositionX(), master->GetPositionY(), master->GetPositionZ(),
                    bot->GetOrientation());

    std::unique_lock<std::shared_mutex> guard(_mutex);
    ++_followedAcrossMaps;

    LOG_DEBUG("playerbots", "[Follow] {} followed {} onto map {}", bot->GetName(), master->GetName(),
              master->GetMapId());
    return true;
}

void BotFollowMgr::Update(Player* bot, uint32 diff)
{
    if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        return;

    ObjectGuid const guid = bot->GetGUID();

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        uint32& timer = _timers[guid];

        if (timer > diff)
        {
            timer -= diff;
            return;
        }

        timer = FOLLOW_INTERVAL_MS;
    }

    Player* master = FollowedMaster(bot);
    if (!master)
        return;

    // A summon first: it is an explicit invitation and carries its own destination, so taking it is
    // always better than inferring one from where the master happens to be standing.
    if (AcceptPendingSummon(bot, master))
        return;

    FollowAcrossMaps(bot, master);
}

std::string BotFollowMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("follow: {} summons accepted, {} map follows, {} refused by the map", _summonsAccepted,
                               _followedAcrossMaps, _refused);
}
