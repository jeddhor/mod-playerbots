/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PostAuctionAction.h"

#include "BotCraftMgr.h"
#include "BotToolMgr.h"

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

        // Keep back what this bot's own recipes consume.
        //
        // ShouldPost answers a question about the market and says so: whether a *particular* bot
        // needs a particular item is a per-bot question it cannot answer. This is that question. A
        // miner who smelts his ore and then auctions every bar has supplied the realm and stranded
        // his own blacksmithing, so a stack is only listed if the reserve survives its going.
        // A shield tank keeps one two-hander for when it goes back to dealing damage.
        //
        // Warriors and paladins only. They are the tanks that put a shield in the off hand, so for
        // them the two-hander is an off-spec weapon the gear scorer now values at a twentieth and
        // the economy would happily sell -- and replacing it costs far more than carrying it.
        // Death knights and druids tank *with* a two-hander; theirs is the main weapon, kept
        // equipped by the ordinary gear logic, and needs no special case here.
        ItemTemplate const* proto = item->GetTemplate();
        bool const shieldTank = bot->getClass() == CLASS_WARRIOR || bot->getClass() == CLASS_PALADIN;

        if (shieldTank && proto->Class == ITEM_CLASS_WEAPON && proto->InventoryType == INVTYPE_2HWEAPON &&
            PlayerbotAI::IsTank(bot, false) && bot->BotCanUseItem(proto) == EQUIP_ERR_OK)
        {
            uint32 held = 0;
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (Item* carried = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (carried->GetTemplate()->InventoryType == INVTYPE_2HWEAPON)
                        ++held;

            if (held <= 1)
                return false;
        }

        // Never auction a tool the bot's own trade needs.
        //
        // The tool manager buys a Virtuoso Inking Set for 750c and the economy promptly listed it:
        // twenty-two were on the house at once, every one of them bought minutes earlier by the
        // scribe now selling it. Money round and round for nothing.
        if (sBotToolMgr.IsNeededTool(bot, item->GetTemplate()->TotemCategory))
            return false;

        uint32 const entry = item->GetTemplate()->ItemId;
        uint32 const reserve = sBotCraftMgr.ReagentReserve(bot, entry);

        if (reserve && bot->GetItemCount(entry, false) - item->GetCount() < reserve)
        {
            LOG_DEBUG("playerbots", "[EconomyDrop] {} kept {} back: reagent reserve {}", bot->GetName(),
                      proto->Name1, reserve);
            return false;
        }

        // Every exit above this one is silent, which is how 287 of one bot's 305 queue attempts
        // disappeared with no way to tell which rule ate them. The manager's own counters covered
        // the market side -- "0 failed, 13281 suppressed by depth" is what finally explained it --
        // but nothing covered the per-bot rules in here.
        if (!sBotEconomyMgr.PostAuction(bot, item))
        {
            LOG_DEBUG("playerbots", "[EconomyDrop] {} could not post {}", bot->GetName(), proto->Name1);
            return false;
        }

        return true;
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
    // looking at -- that was the whole of this rule, and on its own it is too strong. An alt bot
    // that follows you all evening loots the whole time and never sells any of it, so it arrives at
    // a full bag and simply stops: it cannot loot, cannot pick up quest items, and cannot gather.
    //
    // So the rule now has an exception rather than an override. Routine trading while supervised is
    // still off; selling when the bags are actually full is allowed, because at that point the
    // choice is not "sell the owner's things or leave them alone", it is "sell them or let the
    // character stop working". Set Economy.SupervisedBotsSell = 0 to restore the strict rule.
    if (botAI->GetMaster() && !GET_PLAYERBOT_AI(botAI->GetMaster()))
    {
        if (!sPlayerbotAIConfig.economySupervisedBotsSell)
            return false;

        if (bot->GetFreeInventorySpace() >= sPlayerbotAIConfig.economyClearUntilFreeSlots)
            return false;
    }

    // Under the listing cap there is auction work to consider. At the cap there may still be
    // vendoring to do, and a bot with a full auction book and full bags is precisely the bot that
    // most needs to sell something -- refusing to run here is what left five bots on this realm
    // unable to take any action at all.
    if (sBotEconomyMgr.GetBotListingCount(bot->GetGUID()) < sPlayerbotAIConfig.economyMaxListingsPerBot)
        return true;

    return bot->GetFreeInventorySpace() < sPlayerbotAIConfig.agendaFreeSlotTarget;
}

bool PostAuctionAction::Execute(Event /*event*/)
{
    uint32 const held = sBotEconomyMgr.GetBotListingCount(bot->GetGUID());
    uint32 const freeSlots = bot->GetFreeInventorySpace();

    // Two thresholds, not one, because entering and leaving on the same test makes a bot stop the
    // instant it crosses back over it.
    //
    // That is precisely what an operator watched: an alt bot with 112 of 112 slots used cleared its
    // backpack, reached 9 free against a target of 8, and stopped -- with 96 items still sitting in
    // its equipped bags, which it never touched again. Clearing nine slots and calling the job done
    // is not tidying a bag, it is oscillating on a boundary.
    //
    // So `underPressure` (the low mark) only decides how *aggressively* to work, and `shouldTidy`
    // (the high mark) decides whether to work at all. A bot that starts clearing now finishes.
    bool const bagsUnderPressure = freeSlots < sPlayerbotAIConfig.agendaFreeSlotTarget;
    bool const shouldTidy = freeSlots < sPlayerbotAIConfig.economyClearUntilFreeSlots;

    // Two budgets, deliberately separate.
    //
    // Listing is what the auction cap governs. Vendoring is not a listing and must not be rationed
    // by it -- but both used to spend one `slots` allowance computed as (cap - held), so a bot with
    // 23 of 24 listings could take exactly one disposal action per pass, and a bot at 24 returned
    // before the loop and took none. That is the observed failure: an alt bot sitting at 112 of 112
    // bag slots with 23 auctions out, 305 queue attempts, and 18 of its 112 items ever considered.
    //
    // A handful per pass still, because listing a whole bag in one tick floods the house in bursts
    // and reads as a bot. A bot that is actually stuck gets a larger allowance, because pacing
    // matters less than being able to loot at all.
    uint32 listBudget = held < sPlayerbotAIConfig.economyMaxListingsPerBot
                            ? sPlayerbotAIConfig.economyMaxListingsPerBot - held
                            : 0;
    listBudget = std::min<uint32>(listBudget, bagsUnderPressure ? 8 : 3);

    uint32 const vendorBudget = bagsUnderPressure ? 8 : (shouldTidy ? 3 : 1);

    CollectBagItemsVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    uint32 queued = 0;
    uint32 vendored = 0;
    for (Item* item : visitor.items)
    {
        if (queued >= listBudget && vendored >= vendorBudget)
            break;

        if (!sBotEconomyMgr.ShouldPost(item))
        {
            // Bags full and the house already saturated with this item: posting is suppressed and
            // the bot would sit stuck. Take the vendor price and move on. Without this the
            // depth-control mechanism, which exists to protect the market, becomes a way for a bot
            // to deadlock itself.
            // Gear is here alongside trade goods because it can deadlock the same way. Common
            // white drops saturate depth almost immediately -- every bot in a levelling zone loots
            // the same handful of items -- so without this a bot whose bags are full of suppressed
            // white gear has no move at all: it cannot list it, and the vendor path only takes gear
            // that is soulbound.
            // The class list this used to carry -- trade goods, armour, weapons -- left everything
            // else with no way out at all. The bot that prompted this was holding 20 stacks of
            // suppressed white consumables and 6 spare bags, none of which any rule could dispose
            // of, on top of the gear the list did cover.
            //
            // What replaces it is the classifier's own verdict, which is the right question anyway:
            // not "what class is this" but "does this bot still want it". Anything it is keeping,
            // using, wearing, questing with or feeding to a profession is protected; the rest is
            // fair game when the bags are full. Checked here rather than relying on the class list
            // because that list was a proxy for exactly this test.
            if (vendored < vendorBudget && shouldTidy && sBotEconomyMgr.IsAuctionable(item) &&
                item->GetTemplate()->SellPrice)
            {
                ItemUsage const suppressedUsage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
                bool const wanted =
                    suppressedUsage == ITEM_USAGE_EQUIP || suppressedUsage == ITEM_USAGE_REPLACE ||
                    suppressedUsage == ITEM_USAGE_QUEST || suppressedUsage == ITEM_USAGE_SKILL ||
                    suppressedUsage == ITEM_USAGE_USE || suppressedUsage == ITEM_USAGE_GUILD_TASK ||
                    suppressedUsage == ITEM_USAGE_DISENCHANT || suppressedUsage == ITEM_USAGE_KEEP ||
                    suppressedUsage == ITEM_USAGE_AMMO;

                if (!wanted)
                {
                    std::string const name = item->GetTemplate()->Name1;
                    uint32 const earned = sBotEconomyMgr.SellToVendor(bot, item, botAI->HasCheat(BotCheatMask::gold));
                    LOG_DEBUG("playerbots", "[Economy] {} vendored {} to free bag space ({}c)", bot->GetName(), name,
                              earned);
                    ++vendored;
                }
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
            if (vendored >= vendorBudget)
                continue;

            std::string const name = item->GetTemplate()->Name1;
            uint32 const earned = sBotEconomyMgr.SellToVendor(bot, item, botAI->HasCheat(BotCheatMask::gold));

            LOG_DEBUG("playerbots", "[Economy] {} gave up on {} after {} listings, vendored for {}c",
                      bot->GetName(), name, sPlayerbotAIConfig.economyMaxListingAttempts, earned);
            ++vendored;
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

        if (queued >= listBudget)
            continue;

        auto op = std::make_unique<PostAuctionOperation>(bot->GetGUID(), item->GetGUID());
        if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(op)))
            break;  // queue is full; try again next pass rather than spinning

        ++queued;

        LOG_DEBUG("playerbots", "[Economy] {} queued {} for auction", bot->GetName(),
                  item->GetTemplate()->Name1);
    }

    return queued > 0 || vendored > 0;
}
