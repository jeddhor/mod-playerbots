/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PostAuctionAction.h"

#include "BotEconomyMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"

#include <vector>

namespace
{
/// Collects every item currently in the bot's bags so the caller can filter them.
class CollectBagItemsVisitor : public IterateItemsVisitor
{
public:
    bool Visit(Item* item) override
    {
        if (item)
            items.push_back(item);
        return true;
    }

    std::vector<Item*> items;
};

/**
 * Posts one item on the world thread.
 *
 * Carries GUIDs rather than pointers on purpose. Between this being queued on a map thread and run
 * on the world thread the bot can log out and the item can be destroyed, sold or traded away; a
 * raw Item* would be dangling by then. Re-resolving both through the bot means the worst case is
 * that the operation finds nothing and does nothing.
 */
class PostAuctionOperation : public PlayerbotOperation
{
public:
    PostAuctionOperation(ObjectGuid botGuid, ObjectGuid itemGuid) : _botGuid(botGuid), _itemGuid(itemGuid) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(_botGuid);
        if (!bot)
            return false;

        Item* item = bot->GetItemByGuid(_itemGuid);
        if (!item)
            return false;

        // Re-check rather than trusting the queueing thread's decision: depth may have risen past
        // the target, or the bot may have equipped the item, since this was queued.
        if (!sBotEconomyMgr.ShouldPost(item))
            return false;

        return sBotEconomyMgr.PostAuction(bot, item);
    }

    ObjectGuid GetBotGuid() const override { return _botGuid; }
    uint32 GetPriority() const override { return 0; }  // background bookkeeping, never urgent
    std::string GetName() const override { return "PostAuction"; }

private:
    ObjectGuid _botGuid;
    ObjectGuid _itemGuid;
};
}  // namespace

bool PostAuctionAction::isUseful()
{
    if (!sPlayerbotAIConfig.economyEnabled)
        return false;

    // A bot under a human's command should not be quietly liquidating the bags its owner is
    // looking at. Only unsupervised bots trade on their own account.
    if (botAI->GetMaster() && !GET_PLAYERBOT_AI(botAI->GetMaster()))
        return false;

    return sBotEconomyMgr.GetBotListingCount(bot->GetGUID()) < sPlayerbotAIConfig.economyMaxListingsPerBot;
}

bool PostAuctionAction::Execute(Event /*event*/)
{
    uint32 const held = sBotEconomyMgr.GetBotListingCount(bot->GetGUID());
    if (held >= sPlayerbotAIConfig.economyMaxListingsPerBot)
        return false;

    uint32 slots = sPlayerbotAIConfig.economyMaxListingsPerBot - held;

    // A handful per pass. Listing a full 24-slot bag in one tick makes every bot dump its inventory
    // the instant it fills, which reads as a bot and floods the house in bursts. A bot that is
    // actually stuck gets a larger allowance, because pacing matters less than being able to loot.
    slots = std::min<uint32>(slots, bot->GetFreeInventorySpace() < sPlayerbotAIConfig.agendaFreeSlotTarget ? 8 : 3);

    CollectBagItemsVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    bool const bagsUnderPressure = bot->GetFreeInventorySpace() < sPlayerbotAIConfig.agendaFreeSlotTarget;

    uint32 queued = 0;
    for (Item* item : visitor.items)
    {
        if (queued >= slots)
            break;

        if (!sBotEconomyMgr.ShouldPost(item))
        {
            // Bags full and the house already saturated with this item: posting is suppressed and
            // the bot would sit stuck. Take the vendor price and move on. Without this the
            // depth-control mechanism, which exists to protect the market, becomes a way for a bot
            // to deadlock itself.
            if (bagsUnderPressure && sBotEconomyMgr.IsAuctionable(item) &&
                item->GetTemplate()->Class == ITEM_CLASS_TRADE_GOODS && item->GetTemplate()->SellPrice)
            {
                std::string const name = item->GetTemplate()->Name1;
                uint32 const earned = sBotEconomyMgr.SellToVendor(bot, item, botAI->HasCheat(BotCheatMask::gold));
                LOG_DEBUG("playerbots", "[Economy] {} vendored {} to free bag space ({}c)", bot->GetName(), name,
                          earned);
                ++queued;
            }
            continue;
        }

        // An item that has been through several full listings has been declined by the market, not
        // starved of time -- an auction runs at most 48 hours, so five attempts is ten days on the
        // house. Vendoring it recovers something and, more importantly, stops the house
        // accumulating a permanent floor of goods no bot will ever bid on.
        if (sPlayerbotAIConfig.economyMaxListingAttempts &&
            sBotEconomyMgr.GetListingAttempts(item->GetGUID()) >= sPlayerbotAIConfig.economyMaxListingAttempts)
        {
            std::string const name = item->GetTemplate()->Name1;
            uint32 const earned = sBotEconomyMgr.SellToVendor(bot, item, botAI->HasCheat(BotCheatMask::gold));

            LOG_DEBUG("playerbots", "[Economy] {} gave up on {} after {} listings, vendored for {}c",
                      bot->GetName(), name, sPlayerbotAIConfig.economyMaxListingAttempts, earned);
            continue;
        }

        // Defer to the shared classifier for the per-bot half of the judgement. It already knows
        // this bot's quests, professions, ammo and what it is wearing, so only list what it
        // independently agrees is auction fodder rather than something the bot needs.
        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
        if (usage != ITEM_USAGE_AH)
        {
            // Says why a saleable-looking item was passed over. Bots hold cloth, ore and leather
            // and list none of it, and the difference between "never considered" and "considered
            // and classified as something else" is not visible without printing the verdict.
            LOG_DEBUG("playerbots", "[EconomySkip] {} holds {} (class {}) but usage is {}, not AH",
                      bot->GetName(), item->GetTemplate()->Name1, item->GetTemplate()->Class,
                      static_cast<uint32>(usage));
            continue;
        }

        auto op = std::make_unique<PostAuctionOperation>(bot->GetGUID(), item->GetGUID());
        if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(op)))
            break;  // queue is full; try again next pass rather than spinning

        ++queued;

        LOG_DEBUG("playerbots", "[Economy] {} queued {} for auction", bot->GetName(),
                  item->GetTemplate()->Name1);
    }

    return queued > 0;
}
