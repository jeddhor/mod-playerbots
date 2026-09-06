/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BuyAuctionAction.h"

#include "AuctionHouseMgr.h"
#include "BotEconomyMgr.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"

namespace
{
/// Buys one auction on the world thread. GUID/id keyed for the same reason as PostAuctionOperation.
class BuyAuctionOperation : public PlayerbotOperation
{
public:
    BuyAuctionOperation(ObjectGuid botGuid, uint32 auctionId, AuctionHouseId houseId)
        : _botGuid(botGuid), _auctionId(auctionId), _houseId(houseId)
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(_botGuid);
        if (!bot)
            return false;

        return sBotEconomyMgr.BuyoutAuction(bot, _auctionId, _houseId);
    }

    ObjectGuid GetBotGuid() const override { return _botGuid; }
    uint32 GetPriority() const override { return 0; }
    std::string GetName() const override { return "BuyAuction"; }

private:
    ObjectGuid _botGuid;
    uint32 _auctionId;
    AuctionHouseId _houseId;
};
}  // namespace

bool BuyAuctionAction::isUseful()
{
    if (!sPlayerbotAIConfig.economyEnabled)
        return false;

    // Same rule as posting: a bot under a human's command does not spend its owner's gold.
    if (botAI->GetMaster() && !GET_PLAYERBOT_AI(botAI->GetMaster()))
        return false;

    return bot->GetMoney() > 0;
}

bool BuyAuctionAction::Execute(Event /*event*/)
{
    uint32 const money = bot->GetMoney();
    if (!money)
        return false;

    // Never spend the last copper: bots need reserves for repairs, training and travel, and a bot
    // that buys itself broke stops functioning in ways that are hard to trace back to here.
    uint32 const budget = money / 2;
    if (!budget)
        return false;

    std::vector<BotEconomyMgr::Bargain> const bargains =
        sBotEconomyMgr.SampleBargains(bot->GetTeamId(), 15);

    // Counted per pass so a run with zero purchases says which gate closed rather than leaving it
    // to be guessed at.
    uint32 rejOwn = 0, rejBudget = 0, rejOverpriced = 0, rejUnwanted = 0;

    for (BotEconomyMgr::Bargain const& bargain : bargains)
    {
        if (bargain.owner == bot->GetGUID())
        {
            ++rejOwn;
            continue;
        }

        if (bargain.buyout > budget)
        {
            ++rejBudget;
            continue;
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(bargain.itemId);
        if (!proto)
            continue;

        // Don't overpay relative to what the item is actually worth, even if affordable.
        uint32 const value = sBotEconomyMgr.GetMarketPrice(bargain.itemId) * bargain.count;
        if (value && bargain.buyout > value * 11 / 10)
        {
            ++rejOverpriced;
            continue;
        }

        // The shared classifier already knows this bot's class, spec, quest log, professions and
        // what it is currently wearing. Reimplementing that judgement here would drift out of sync
        // with how the bot evaluates the same item when it loots one.
        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", bargain.itemId);
        bool const wanted = usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE ||
                            usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_USE ||
                            usage == ITEM_USAGE_QUEST || usage == ITEM_USAGE_AMMO;

        // An enchanter buys gear to break down for dust and essences. This is the one case where a
        // bot buys something it has no direct use for, and it is what actually clears gear off the
        // house: items nobody wants to wear would otherwise sit until they expire.
        //
        // Judged against what the reagents are actually worth, computed from disenchant_loot_template
        // against the live index -- not against a discount off the item's own price. The discount
        // test could never fire: bots price at market, and the lowest a posted price can reach is
        // about 0.55 of the index, so "at most half of market" excluded every listing that existed.
        uint32 const deValue = sBotEconomyMgr.GetDisenchantValue(proto->DisenchantID);
        bool const forDisenchant =
            usage == ITEM_USAGE_DISENCHANT && deValue &&
            bargain.buyout <= deValue * sPlayerbotAIConfig.economyDisenchantMaxPricePct / 100;

        if (!wanted && !forDisenchant)
        {
            ++rejUnwanted;
            continue;
        }

        auto op = std::make_unique<BuyAuctionOperation>(bot->GetGUID(), bargain.auctionId, bargain.houseId);
        if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(op)))
            return false;

        LOG_DEBUG("playerbots", "[Economy] {} queued buyout of {}x{} for {}c (usage {}{})", bot->GetName(),
                  bargain.count, proto->Name1, bargain.buyout, static_cast<uint32>(usage),
                  forDisenchant ? ", to disenchant" : "");

        // One purchase per pass: a bot that empties the house in a single tick is both obvious and
        // a good way to corner the market by accident.
        return true;
    }

    LOG_DEBUG("playerbots",
              "[BuyScan] {} saw {} bargains, budget {}c: {} own, {} unaffordable, {} overpriced, {} unwanted",
              bot->GetName(), bargains.size(), budget, rejOwn, rejBudget, rejOverpriced, rejUnwanted);

    return false;
}
