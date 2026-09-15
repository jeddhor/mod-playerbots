/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTCRAFTMGR_H
#define PLAYERBOTS_BOTCRAFTMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Item;
class Player;
class SpellInfo;

/**
 * Bots craft the things other bots' professions depend on, and sell them.
 *
 * Enchanting is the case that forces this. An enchanter needs a runed rod, no vendor sells one, and
 * an enchanter cannot make the blank it is runed from -- a blacksmith makes that. So an enchanter on
 * a realm of bots is stuck at the first rod unless some blacksmith decides to supply the market.
 *
 * What counts as a supply item is derived rather than listed. A tool is any item with a
 * TotemCategory; the reagents of the spells that create tools are therefore the blanks the tool
 * chain depends on. That reaches every rod tier without naming one, and would reach a tool added by
 * a later patch on its own.
 *
 * Supply is demand-led: a bot crafts a blank only when the auction house is short of it, so this
 * does not turn every blacksmith into a rod factory flooding the market with copper rods.
 */
class BotCraftMgr
{
public:
    static BotCraftMgr& instance()
    {
        static BotCraftMgr instance;
        return instance;
    }

    /// Per-bot tick. Cheap except on the periodic pass.
    void Update(Player* bot, uint32 diff);

    /// True if this item is a reagent of some tool-creating recipe, and so worth stocking.
    bool IsToolBlank(uint32 itemId);

    /**
     * How many of an item this bot should keep back for its own crafting.
     *
     * A miner who smelts his ore into bars and then auctions every bar has done his blacksmithing
     * no favours. Anything a known recipe consumes is reserved up to a few crafts' worth; the
     * surplus above that is what the economy is allowed to sell.
     */
    uint32 ReagentReserve(Player* bot, uint32 itemId);

    /// Turn raw materials into the refined form the bot can actually use or sell. Returns crafts made.
    uint32 RefineMaterials(Player* bot);

    /**
     * Craft one unit of the item this spell makes, consuming its reagents. False if short.
     *
     * Public because the craft-goal activity is a second legitimate caller: a bot that has spent
     * twenty minutes farming the reagents needs to make the thing, and routing that through this
     * manager keeps one implementation of "consume reagents, grant the skill, announce the item"
     * rather than two that drift.
     */
    bool CraftOne(Player* bot, uint32 spellId, uint32 itemId, bool forMarket);

    /// True if `itemId` is an enchant scroll this bot could put on a piece of gear it is wearing now.
    /// The item classifier asks, so a bot keeps a scroll it bought instead of listing it again.
    bool IsScrollUsefulTo(Player* bot, uint32 itemId);

    /// True if `itemId` is a herb this scribe can mill. Milling is not a recipe reagent, so nothing
    /// else reserves herbs for a scribe -- the economy sold every one it gathered.
    bool IsHerbToMill(Player* bot, uint32 itemId);

    /// True if `itemId` is vellum and the bot is an enchanter who writes scrolls onto it.
    bool IsVellumFor(Player* bot, uint32 itemId);

    std::string DescribeStats() const;

private:
    BotCraftMgr() = default;
    ~BotCraftMgr() = default;

    BotCraftMgr(BotCraftMgr const&) = delete;
    BotCraftMgr& operator=(BotCraftMgr const&) = delete;

    /// Walk every spell once to find which items are tools, and what those tools are made from.
    void EnsureLoaded();

    /// Buy a missing reagent off the auction house, or from a vendor if one stocks it for gold.
    /// False if neither has it or it is unaffordable.
    bool BuyReagent(Player* bot, uint32 itemId, uint32 needed);

    /// Make a reagent from what is already in the bag, if the bot knows a recipe for it. With `dryRun`
    /// only reports whether it could.
    bool CraftReagent(Player* bot, uint32 itemId, bool dryRun = false);

    /// Held already, or to be had: a vendor stocks it, the house lists it, or the bag can make it.
    bool IsObtainable(Player* bot, uint32 itemId, uint32 needed);

    /// Every reagent of this recipe is obtainable -- the test before spending anything on one of them.
    bool AllReagentsObtainable(Player* bot, SpellInfo const* info);

    /// Buy from a vendor without travelling, the same abstraction training and vendoring use: the
    /// errand is skipped, the price is not. Only trade goods a vendor stocks for gold.
    bool BuyFromVendor(Player* bot, uint32 itemId, uint32 needed, uint32 budget);

    /**
     * Mill herb stacks into pigments -- the first link of Inscription, and the one nothing did.
     *
     * Pigments feed inks, inks feed vellum, vellum feeds scrolls. Without this a scribe holding a bag
     * of herbs could make none of it, and the scroll trade had no bottom rung.
     */
    uint32 MillHerbs(Player* bot);

    /// The cheapest vellum this enchant can be written onto that the bot can get hold of, or 0.
    uint32 VellumFor(Player* bot, SpellInfo const* enchant);

    /// The equipped item a scroll's enchant would go on, or nullptr.
    Item* ScrollTarget(Player* bot, SpellInfo const* enchant) const;

    /**
     * Write one scroll for the market, buying its vellum or a missing reagent first if need be.
     *
     * Demand-led like the tool blanks, and gated on price: a scroll is written only when what it
     * sells for covers the vellum and dust that go into it.
     */
    bool SupplyScroll(Player* bot);

    /// Buy one scroll for an unenchanted piece of gear the bot wears (P10.7).
    bool BuyScroll(Player* bot);

    /// Put any scroll the bot holds onto the gear it fits. Returns scrolls used.
    uint32 ApplyScrolls(Player* bot);

    /**
     * Craft one recipe that would still raise a profession skill, from reagents already in the bag.
     *
     * This is the half of crafting that was missing: the market loop above only ever makes tool
     * blanks, so a bot with a known recipe, the reagents for it, and a skill that recipe would
     * raise still never crafted. The visible form of that was a paladin holding twenty Stringy Wolf
     * Meat that ReagentReserve kept back *for* cooking, which nothing then cooked -- the bag space
     * spent and no skill gained.
     *
     * Reagents are never bought for this. Buying to level a skill is a gold sink with no economic
     * signal behind it, and a bot that starts doing it keeps doing it until the gold is gone.
     */
    bool CraftForSkillUp(Player* bot);

    /**
     * Buy the materials for a known recipe that makes a genuine upgrade, and craft it.
     *
     * This is the consumption side of the economy. Everything else in the market is bots *selling*
     * -- gathered ore, looted greens, surplus bars -- and a market with only sellers is a market
     * whose prices fall until nothing is worth gathering. This is the one rule that makes a bot buy
     * raw materials for a reason it would have anyway: it wants better gear.
     *
     * Deliberately the only place reagents are bought for the bot's own use. CraftForSkillUp will
     * not buy, because "I want a skill point" is not a reason a market can price.
     */
    bool CraftUpgrade(Player* bot);

    std::once_flag _loadOnce;

    /// Items that are reagents of a tool recipe -- rod blanks and their equivalents -- plus vellum,
    /// which is the same shape of dependency: enchanting consumes it, only inscription makes it.
    std::unordered_set<uint32> _toolBlanks;

    /// Scroll item -> the enchant spell that writes it, and the reverse.
    std::unordered_map<uint32, uint32> _scrollSpell;
    std::unordered_map<uint32, uint32> _spellScroll;

    /// (item level, entry) of each vellum, by kind, cheapest tier first.
    std::vector<std::pair<uint32, uint32>> _armorVellums;
    std::vector<std::pair<uint32, uint32>> _weaponVellums;

    /// Herbs that can be milled, and the spells that mill.
    std::unordered_set<uint32> _millable;
    std::unordered_set<uint32> _millingSpells;

    /// Trade goods some vendor sells for plain gold (no extended cost).
    std::unordered_set<uint32> _vendorGoods;

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, uint32> _timers;

    uint32 _crafted{0};
    uint32 _skillCrafts{0};
    uint32 _upgradeCrafts{0};
    uint32 _listed{0};
    uint32 _shortReagents{0};
    uint32 _reagentsBought{0};
    uint32 _vendorBought{0};
    uint32 _milled{0};
    uint32 _scrollsWritten{0};
    uint32 _scrollsBought{0};
    uint32 _scrollsApplied{0};
};

#define sBotCraftMgr BotCraftMgr::instance()

#endif
