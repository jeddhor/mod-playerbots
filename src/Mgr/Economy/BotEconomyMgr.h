/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTECONOMYMGR_H
#define PLAYERBOTS_BOTECONOMYMGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

enum class AuctionHouseId : uint8;

class Item;
class Player;
struct ItemTemplate;

/**
 * The bot-facing side of the auction house economy.
 *
 * Bots post what they gather and loot, and buy what they need, without travelling to an auctioneer
 * and without paying a deposit (see DESIGN section 1.1 — both are deliberate simplifications the
 * operator asked for). Everything else goes through the core's own auction path, so a real player
 * standing at an auctioneer sees bot listings, can outbid them, and gets paid by the same mail that
 * pays every other seller.
 *
 * Three things live here:
 *
 * 1. A **price index**. Seeded from `item_template` and then moved by an exponential moving average
 *    of the lowest per-unit buyout actually observed on the house. A bot that has never seen an item
 *    trade still gets a sane asking price; one that has seen it a hundred times asks what the market
 *    bears. Persisted to `playerbot_market_price`.
 *
 * 2. **Depth control**. The listing count per item at the last sample. Posting is suppressed above a
 *    target depth and the price is scaled down as depth rises. This is the negative feedback that
 *    stops the house filling up with 4000 stacks of Copper Ore priced identically — without it,
 *    posting is a one-way valve and the market is a static wall.
 *
 * 3. **Posting**. `PostAuction` mirrors `WorldSession::HandleAuctionSellItem` minus the proximity
 *    and deposit checks.
 *
 * Threading: `PostAuction` and `ObserveMarket` are **world thread only** — `AuctionHouseObject::
 * AddAuction` feeds the multi-threaded `AuctionHouseSearcher`, and the core only ever calls it from
 * packet handlers, which run on the world thread. Price *reads* happen from bot AI and are served
 * from a lock-free published snapshot.
 */
class BotEconomyMgr
{
public:
    static BotEconomyMgr& instance()
    {
        static BotEconomyMgr instance;
        return instance;
    }

    /// Load the persisted price index. Call once, during world startup.
    void Load();

    /// Slow tick. Samples the auction houses and updates the price index. World thread only.
    void Update(uint32 diff);

    /**
     * Estimated market value of one unit of `itemId`, in copper.
     *
     * Never returns 0 for a real item: an item the house has never carried falls back to the
     * vendor-derived seed, so a bot can always price what it is holding.
     */
    uint32 GetMarketPrice(uint32 itemId) const;

    /// Listings of `itemId` seen on the last sampling pass.
    uint32 GetListingDepth(uint32 itemId) const;

    /**
     * Auctions this bot currently has up.
     *
     * Resynced from the live houses on every sampling pass and incremented on each post, so it
     * tracks sales and expiries without having to walk every auction each time a bot wants to list
     * something.
     */
    uint32 GetBotListingCount(ObjectGuid owner) const;

    /// True if this item may be listed at all — BoE, not soulbound, not a quest or conjured item.
    static bool IsAuctionable(Item* item);

    /**
     * Whether a bot should list this item right now, given what the house already holds.
     * Separate from IsAuctionable so callers can distinguish "never postable" from "not worth
     * posting at the moment".
     */
    bool ShouldPost(Item* item) const;

    /**
     * List the whole of `item`'s stack on the bot's faction auction house.
     *
     * Whole stacks only — the partial-stack path in the core clones the item, and a clone that
     * escapes its transaction is an item duplication bug. Bots post full stacks anyway.
     *
     * @return true if the auction was created. On false the bot's bags are untouched.
     */
    bool PostAuction(Player* bot, Item* item);

    /**
     * A listing worth a second look: priced at or below what the index says the item is worth.
     *
     * Bots sample this shortlist instead of scanning the whole house. With a few thousand bots each
     * looking for something to buy, walking every auction per bot is the difference between a
     * feature and a stall.
     */
    struct Bargain
    {
        uint32 auctionId;
        AuctionHouseId houseId;
        uint32 itemId;
        uint32 count;
        uint32 buyout;
        ObjectGuid owner;
    };

    /// Up to `limit` randomly sampled bargains visible to `team`.
    std::vector<Bargain> SampleBargains(TeamId team, uint32 limit) const;

    /**
     * Buy out an existing auction on the bot's behalf.
     *
     * Goes through the core's own settlement so the seller is paid by the same mail any player
     * would trigger. World thread only.
     */
    bool BuyoutAuction(Player* bot, uint32 auctionId, AuctionHouseId houseId);

    struct Stats
    {
        std::atomic<uint32> posted{0};
        std::atomic<uint32> postFailed{0};
        std::atomic<uint32> suppressed{0};
        std::atomic<uint32> bought{0};
        std::atomic<uint64> goldListed{0};
        std::atomic<uint64> goldSpent{0};
    };

    Stats const& GetStats() const { return _stats; }

    /// Snapshot of index size and total listings, for `.playerbots economy`.
    void GetIndexSummary(uint32& items, uint32& listings) const;

    /// Dump the state of the bot economy to the log: listings, flows, and the realm's bot gold.
    void PrintStats();

private:
    BotEconomyMgr() = default;
    ~BotEconomyMgr() = default;

    BotEconomyMgr(BotEconomyMgr const&) = delete;
    BotEconomyMgr& operator=(BotEconomyMgr const&) = delete;

    struct PriceRecord
    {
        uint32 price{0};
        uint32 depth{0};
        uint32 observations{0};
    };

    using PriceMap = std::unordered_map<uint32, PriceRecord>;

    /// Vendor-and-quality derived opening estimate for an item the house has never carried.
    static uint32 SeedPrice(ItemTemplate const* proto);

    /// Walk every auction house, fold observed buyouts into the index, republish. World thread only.
    void ObserveMarket();

    /// Write the index back to `playerbot_market_price`. Asynchronous.
    void Persist();

    // Write side. Only touched by ObserveMarket/Load on the world thread, but guarded because
    // Persist() reads it to build its batch.
    mutable std::mutex _mutex;
    PriceMap _records;

    // Read side: a lock-free snapshot republished after each sampling pass. Bots read prices while
    // deciding what to post, which is far more often than the index changes.
    std::atomic<std::shared_ptr<PriceMap const>> _published;

    // Rebuilt from the live houses each sampling pass; incremented immediately on a post so a bot
    // cannot blow past its cap in the interval between two passes.
    std::unordered_map<ObjectGuid, uint32> _listingsByOwner;

    // Refreshed each sampling pass and read as a lock-free snapshot, same as the price index.
    using BargainList = std::vector<Bargain>;
    std::atomic<std::shared_ptr<BargainList const>> _bargains;

    uint32 _sampleTimer{0};
    uint32 _persistTimer{0};

    // mutable so ShouldPost, which is a query, can still record why it said no.
    mutable Stats _stats;
};

#define sBotEconomyMgr BotEconomyMgr::instance()

#endif
