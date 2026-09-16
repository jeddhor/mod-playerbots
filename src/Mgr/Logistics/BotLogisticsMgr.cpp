/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotLogisticsMgr.h"

#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringFormat.h"

namespace
{
/// Often enough that a bag never stays full for long, rare enough to be invisible beside the AI.
constexpr uint32 LOGISTICS_INTERVAL_MS = 30 * 1000;
}  // namespace

void BotLogisticsMgr::Update(Player* bot, uint32 diff)
{
    if (!bot || !bot->IsInWorld())
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    // Not mid-fight: selling is not urgent, and a bot rummaging through its bags while something is
    // hitting it is neither useful nor what a person would do.
    if (bot->IsInCombat() || bot->IsBeingTeleported())
        return;

    // And not while their owner is steering. Someone moving their character expects it to move, not
    // to stop and start dealing with the auction house.
    if (botAI->HumanIsDriving())
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

        timer = LOGISTICS_INTERVAL_MS;
        ++_passes;
    }

    // Junk first, and unconditionally: greys are meant to be sold where the bot stands, with no
    // vendor trip and no bag-pressure test, so a bot never carries them at all. That was already the
    // rule; nothing was ever asking for it often enough to be true.
    //
    // Silent, because DoSpecificAction otherwise whispers the outcome to the bot's master, and an
    // owner does not need to be told about every grey pelt.
    if (botAI->DoSpecificAction("vendor junk", Event(), true))
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_junkPasses;
    }

    // Then the auction pass, which decides for itself whether there is anything to do: under the
    // listing cap it looks for things worth listing, and when the bags are full it will also take
    // the vendor price for what the house will not accept. Both of those live in the action.
    if (botAI->DoSpecificAction("post auctions", Event(), true))
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_auctionPasses;
    }
}

std::string BotLogisticsMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("logistics: {} passes, {} sold junk, {} worked the auction house", _passes,
                               _junkPasses, _auctionPasses);
}
