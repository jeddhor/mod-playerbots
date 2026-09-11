/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotEconomyMgr.h"

#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "ScriptMgr.h"
#include "World.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    // How hard a single sampling pass pulls the stored price toward what was observed. Low, because
    // one pass is one noisy snapshot of a thin market, not a verdict.
    constexpr float PRICE_ALPHA = 0.25f;

    constexpr uint32 AUCTION_DURATIONS[] = {12 * HOUR, 24 * HOUR, 48 * HOUR};

    // The core refuses more than this many auctions per owner; exceeding it desyncs the owner's
    // client-side auction list.
    constexpr uint32 CORE_MAX_AUCTION_ITEMS = 160;
}

void BotEconomyMgr::Load()
{
    uint32 const oldMSTime = getMSTime();

    PriceMap loaded;
    if (QueryResult result =
            PlayerbotsDatabase.Query("SELECT item_id, price, depth, observations FROM playerbot_market_price"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const itemId = fields[0].Get<uint32>();

            PriceRecord record;
            record.price = fields[1].Get<uint32>();
            record.depth = fields[2].Get<uint32>();
            record.observations = fields[3].Get<uint32>();
            loaded[itemId] = record;
        } while (result->NextRow());
    }

    std::unordered_map<uint32, uint32> attempts;
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT item_guid, attempts FROM playerbot_auction_attempts"))
    {
        do
        {
            Field* fields = result->Fetch();
            attempts[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (result->NextRow());
    }

    {
        std::lock_guard<std::mutex> guard(_mutex);
        _listingAttempts = std::move(attempts);
        _records = std::move(loaded);
        _published.store(std::make_shared<PriceMap const>(_records), std::memory_order_release);
    }

    LoadDisenchantYields();

    LOG_INFO("server.loading", ">> Loaded {} playerbot market prices in {} ms",
             _published.load(std::memory_order_acquire)->size(), GetMSTimeDiffToNow(oldMSTime));
}

void BotEconomyMgr::LoadDisenchantYields()
{
    // Rows within a loot group share the probability: entries with an explicit Chance take it, and
    // any left at zero split whatever remains of the group equally. Reading Chance 0 as "never
    // drops" would undervalue every disenchant, because the commonest reagents are exactly the ones
    // stored that way.
    struct Row
    {
        uint32 itemId;
        float chance;
        float avgCount;
        uint32 groupId;
    };

    std::unordered_map<uint32, std::vector<Row>> byEntry;

    if (QueryResult result = WorldDatabase.Query(
            "SELECT Entry, Item, Chance, MinCount, MaxCount, GroupId FROM disenchant_loot_template"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const entry = fields[0].Get<uint32>();
            Row row;
            row.itemId = fields[1].Get<uint32>();
            row.chance = fields[2].Get<float>();
            row.avgCount = (fields[3].Get<uint8>() + fields[4].Get<uint8>()) / 2.0f;
            row.groupId = fields[5].Get<uint8>();
            byEntry[entry].push_back(row);
        } while (result->NextRow());
    }

    for (auto& [entry, rows] : byEntry)
    {
        // Distribute each group's unclaimed probability across its zero-chance rows.
        std::unordered_map<uint32, float> claimed;
        std::unordered_map<uint32, uint32> blanks;
        for (Row const& row : rows)
        {
            claimed[row.groupId] += row.chance;
            if (row.chance <= 0.0f)
                ++blanks[row.groupId];
        }

        std::vector<Yield> yields;
        yields.reserve(rows.size());

        for (Row const& row : rows)
        {
            float chance = row.chance;
            if (chance <= 0.0f && blanks[row.groupId])
                chance = std::max(0.0f, 100.0f - claimed[row.groupId]) / blanks[row.groupId];

            if (chance <= 0.0f)
                continue;

            yields.push_back({row.itemId, (chance / 100.0f) * row.avgCount});
        }

        if (!yields.empty())
            _disenchantYields[entry] = std::move(yields);
    }

    LOG_INFO("server.loading", ">> Loaded disenchant yields for {} item classes", _disenchantYields.size());
}

uint32 BotEconomyMgr::GetDisenchantValue(uint32 disenchantId) const
{
    if (!disenchantId)
        return 0;

    auto itr = _disenchantYields.find(disenchantId);
    if (itr == _disenchantYields.end())
        return 0;

    float value = 0.0f;
    for (Yield const& yield : itr->second)
        value += yield.expectedCount * GetMarketPrice(yield.itemId);

    return static_cast<uint32>(value);
}

uint32 BotEconomyMgr::SeedPrice(ItemTemplate const* proto)
{
    if (!proto)
        return 0;

    // What a vendor pays is the hard floor on what anything is worth: below it a seller would
    // simply vendor the item instead of listing it.
    uint32 base = proto->SellPrice;
    if (!base && proto->BuyPrice)
        base = proto->BuyPrice / 4;
    if (!base)
        base = 1 + (proto->ItemLevel * proto->ItemLevel) / 8;

    float multiplier;
    switch (proto->Quality)
    {
        case ITEM_QUALITY_POOR:
            multiplier = 1.0f;
            break;
        case ITEM_QUALITY_NORMAL:
            multiplier = 2.0f;
            break;
        case ITEM_QUALITY_UNCOMMON:
            multiplier = 5.0f;
            break;
        case ITEM_QUALITY_RARE:
            multiplier = 12.0f;
            break;
        case ITEM_QUALITY_EPIC:
            multiplier = 30.0f;
            break;
        default:
            multiplier = 3.0f;
            break;
    }

    // Reagents and recipes clear well above vendor value because they are an input to something
    // else, not an end product someone happens to be holding.
    if (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_RECIPE)
        multiplier *= 1.5f;

    return std::max<uint32>(1, static_cast<uint32>(base * multiplier));
}

uint32 BotEconomyMgr::GetMarketPrice(uint32 itemId) const
{
    if (std::shared_ptr<PriceMap const> snapshot = _published.load(std::memory_order_acquire))
    {
        auto itr = snapshot->find(itemId);
        if (itr != snapshot->end() && itr->second.price)
            return itr->second.price;
    }

    return SeedPrice(sObjectMgr->GetItemTemplate(itemId));
}

uint32 BotEconomyMgr::GetListingDepth(uint32 itemId) const
{
    if (std::shared_ptr<PriceMap const> snapshot = _published.load(std::memory_order_acquire))
    {
        auto itr = snapshot->find(itemId);
        if (itr != snapshot->end())
            return itr->second.depth;
    }

    return 0;
}

uint32 BotEconomyMgr::GetListingAttempts(ObjectGuid itemGuid) const
{
    std::lock_guard<std::mutex> guard(_mutex);
    auto itr = _listingAttempts.find(itemGuid.GetCounter());
    return itr != _listingAttempts.end() ? itr->second : 0;
}

uint32 BotEconomyMgr::SellToVendor(Player* bot, Item* item, bool goldCheat)
{
    if (!bot || !item)
        return 0;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return 0;

    uint32 const price = proto->SellPrice * item->GetCount();
    uint32 const itemGuid = item->GetGUID().GetCounter();

    bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);

    // With the gold cheat active a bot's money is held constant, so cheat realms do not quietly
    // inflate every time a bot clears its bags.
    if (!goldCheat)
        bot->ModifyMoney(static_cast<int32>(price));

    {
        std::lock_guard<std::mutex> guard(_mutex);
        _listingAttempts.erase(itemGuid);
    }
    PlayerbotsDatabase.Execute("DELETE FROM playerbot_auction_attempts WHERE item_guid = {}", itemGuid);

    return price;
}

uint32 BotEconomyMgr::GetBotListingCount(ObjectGuid owner) const
{
    std::lock_guard<std::mutex> guard(_mutex);
    auto itr = _listingsByOwner.find(owner);
    return itr != _listingsByOwner.end() ? itr->second : 0;
}

void BotEconomyMgr::GetIndexSummary(uint32& items, uint32& listings) const
{
    items = 0;
    listings = 0;

    if (std::shared_ptr<PriceMap const> snapshot = _published.load(std::memory_order_acquire))
    {
        items = static_cast<uint32>(snapshot->size());
        for (auto const& [itemId, record] : *snapshot)
            listings += record.depth;
    }
}

void BotEconomyMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.economyEnabled)
        return;

    _sampleTimer += diff;
    if (_sampleTimer < sPlayerbotAIConfig.economySampleIntervalSeconds * IN_MILLISECONDS)
        return;

    _sampleTimer = 0;
    ObserveMarket();

    // Persisting every sample would write the whole index every few minutes for no benefit; the
    // index only has to survive a restart, not be durable to the second.
    if (++_persistTimer >= 4)
    {
        _persistTimer = 0;
        Persist();
    }

    // The same summary `.playerbots economy` prints, on a slow cycle. An operator watching a realm
    // overnight should not have to be at a console to answer "is gold inflating, and is the market
    // clearing?", and it means the reporting path is exercised on every run rather than only when
    // somebody remembers to ask for it.
    if (++_reportTimer >= 10)
    {
        _reportTimer = 0;
        PrintStats();
    }
}

void BotEconomyMgr::ObserveMarket()
{
    uint32 const oldMSTime = getMSTime();

    // Per-unit buyouts seen this pass, per item.
    std::unordered_map<uint32, std::vector<uint32>> observed;
    std::unordered_map<ObjectGuid, uint32> owners;
    auto bargains = std::make_shared<BargainList>();

    AuctionHouseId const houses[] = {AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral};
    AuctionHouseObject* seen[3] = {nullptr, nullptr, nullptr};
    uint32 seenCount = 0;
    uint32 totalAuctions = 0;

    for (AuctionHouseId houseId : houses)
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
        if (!house)
            continue;

        // With two-side interaction enabled every id resolves to the same neutral house; sampling
        // it three times would treble every depth reading.
        bool duplicate = false;
        for (uint32 i = 0; i < seenCount; ++i)
            if (seen[i] == house)
                duplicate = true;
        if (duplicate)
            continue;
        seen[seenCount++] = house;

        for (auto const& [auctionId, auction] : house->GetAuctions())
        {
            if (!auction || !auction->buyout || !auction->itemCount)
                continue;

            uint32 const unitPrice = auction->buyout / auction->itemCount;
            observed[auction->item_template].push_back(unitPrice);
            ++owners[auction->owner];
            ++totalAuctions;

            // Judged against the index as it stood at the start of this pass, which is the same
            // number the seller priced against. Anything at or under it is worth a bot's attention.
            if (unitPrice <= GetMarketPrice(auction->item_template))
                bargains->push_back({auction->Id, auction->GetHouseId(), auction->item_template,
                                     auction->itemCount, auction->buyout, auction->owner});
        }
    }

    std::lock_guard<std::mutex> guard(_mutex);

    // Authoritative resync: sold and expired auctions have left the house, so a bot's slot count
    // must be rebuilt from what is actually listed rather than decremented on guesswork.
    _listingsByOwner = std::move(owners);

    // Depth is a property of the current listings, so an item that has fallen off the house
    // entirely must drop to zero rather than keep its last reading forever.
    for (auto& [itemId, record] : _records)
        record.depth = 0;

    for (auto& [itemId, prices] : observed)
    {
        // The median, not the minimum. Bots price from this index and then undercut it slightly, so
        // folding the *lowest* listing back in each pass would ratchet every price toward zero — a
        // feedback loop with no floor, since the sellers and the buyers are the same population.
        std::nth_element(prices.begin(), prices.begin() + prices.size() / 2, prices.end());
        uint32 observedPrice = prices[prices.size() / 2];

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);

        // No item trades below vendor price: a seller would vendor it instead. This is the hard
        // floor that stops any residual downward drift.
        if (proto && proto->SellPrice)
            observedPrice = std::max(observedPrice, proto->SellPrice);

        PriceRecord& record = _records[itemId];
        record.depth = static_cast<uint32>(prices.size());
        record.price = record.observations
                           ? static_cast<uint32>(record.price * (1.0f - PRICE_ALPHA) + observedPrice * PRICE_ALPHA)
                           : observedPrice;
        ++record.observations;
    }

    _published.store(std::make_shared<PriceMap const>(_records), std::memory_order_release);
    _bargains.store(bargains, std::memory_order_release);

    LOG_DEBUG("playerbots", "[Economy] sampled {} auctions, {} distinct items, index holds {} ({} ms)",
              totalAuctions, observed.size(), _records.size(), GetMSTimeDiffToNow(oldMSTime));
    LOG_DEBUG("playerbots", "[Economy] {} listings priced at or below the index", bargains->size());
}

void BotEconomyMgr::Persist()
{
    // One multi-row upsert per chunk rather than a statement per item: the index can hold tens of
    // thousands of entries and this runs on the world thread.
    //
    // Every value interpolated here is an integer read out of our own map, so there is no injection
    // surface; the playerbots pool has no prepared statement for this module-owned table, and adding
    // one would mean editing the core's statement enum for a table the core knows nothing about.
    constexpr size_t CHUNK = 500;

    std::vector<std::string> rows;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        rows.reserve(_records.size());
        for (auto const& [itemId, record] : _records)
        {
            if (!record.observations)
                continue;

            rows.push_back(Acore::StringFormat("({},{},{},{})", itemId, record.price, record.depth,
                                               record.observations));
        }
    }

    for (size_t offset = 0; offset < rows.size(); offset += CHUNK)
    {
        std::string sql =
            "INSERT INTO playerbot_market_price (item_id, price, depth, observations) VALUES ";

        size_t const last = std::min(offset + CHUNK, rows.size());
        for (size_t i = offset; i < last; ++i)
        {
            if (i != offset)
                sql += ',';
            sql += rows[i];
        }

        sql +=
            " ON DUPLICATE KEY UPDATE price = VALUES(price), depth = VALUES(depth), "
            "observations = VALUES(observations)";

        PlayerbotsDatabase.Execute(sql);
    }
}

bool BotEconomyMgr::IsAuctionable(Item* item)
{
    if (!item)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return false;

    // Soulbound is the decisive test — it covers both BIND_WHEN_PICKED_UP items and BoE items the
    // bot has already equipped, neither of which can legally be listed.
    if (item->IsSoulBound() || proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM)
        return false;

    if (proto->Class == ITEM_CLASS_QUEST)
        return false;

    if (proto->Flags & ITEM_FLAG_CONJURED)
        return false;

    // Anything mid-auction, being traded, or otherwise not freely in the bag.
    if (item->IsInTrade() || !item->CanBeTraded())
        return false;

    if (item->GetCount() == 0)
        return false;

    return true;
}

bool BotEconomyMgr::ShouldPost(Item* item) const
{
    if (!IsAuctionable(item))
        return false;

    ItemTemplate const* proto = item->GetTemplate();

    // Greys go to a vendor, not the auction house — nobody bids on them and they would bury every
    // real listing. VendorJunkAction (Phase 3) already handles them without travel.
    if (proto->Quality < ITEM_QUALITY_NORMAL)
        return false;

    // White items are listable when they are an input to something -- reagents and recipes -- and
    // when they are gear.
    //
    // Gear was excluded on the grounds that a white sword is vendor fodder. That was true while the
    // only two outcomes were "listed forever" or "vendored", but PostAuctionAction now gives up
    // after Economy.MaxListingAttempts listings and vendors the item, so an unwanted white sword
    // leaves the house by itself. Excluding it here left unbound white gear with nowhere to go at
    // all: not listable, and not sellable either, because the vendor path only takes gear that is
    // soulbound. It just accumulated -- one level 16 character was carrying thirty-five pieces.
    //
    // Consumables are deliberately NOT here. They were once, and bots promptly auctioned their own
    // healing potions, poisons and sharpening stones -- every white listing in the first economy
    // run was a consumable the bot should have been drinking. Whether a *particular* bot needs a
    // particular consumable is a per-bot question this class cannot answer; the caller asks
    // ItemUsageValue.
    if (proto->Quality == ITEM_QUALITY_NORMAL && proto->Class != ITEM_CLASS_TRADE_GOODS &&
        proto->Class != ITEM_CLASS_RECIPE && proto->Class != ITEM_CLASS_ARMOR &&
        proto->Class != ITEM_CLASS_WEAPON)
        return false;

    // Depth control: above the target the house already has more of this than it can clear, so
    // adding to the pile only wastes the bot's listing slots.
    if (GetListingDepth(proto->ItemId) >= sPlayerbotAIConfig.economyTargetDepth)
    {
        ++_stats.suppressed;
        return false;
    }

    return true;
}

bool BotEconomyMgr::PostAuction(Player* bot, Item* item)
{
    if (!bot || !IsAuctionable(item))
    {
        ++_stats.postFailed;
        return false;
    }

    ItemTemplate const* proto = item->GetTemplate();

    AuctionHouseId const teamHouse =
        bot->GetTeamId() == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde;

    // Both of these normalise to the neutral house when two-side interaction is enabled.
    AuctionHouseEntry const* ahEntry = AuctionHouseMgr::GetAuctionHouseEntryFromHouse(teamHouse);
    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(teamHouse);
    if (!ahEntry || !house)
    {
        ++_stats.postFailed;
        return false;
    }

    uint32 const count = item->GetCount();
    if (!count)
    {
        ++_stats.postFailed;
        return false;
    }

    uint32 const unitValue = GetMarketPrice(proto->ItemId);
    if (!unitValue)
    {
        ++_stats.postFailed;
        return false;
    }

    // Price falls as the house fills up. Without this every bot posts the same number and the
    // auction house becomes a static wall that never clears.
    uint32 const depth = GetListingDepth(proto->ItemId);
    float const targetDepth = std::max<float>(1.0f, static_cast<float>(sPlayerbotAIConfig.economyTargetDepth));
    float const depthFactor = std::clamp(1.0f - depth / targetDepth, 0.6f, 1.4f);

    // Bots are not a cartel; identical pricing across thousands of listings reads as obviously
    // artificial to a player browsing the house.
    float const noise = frand(0.92f, 1.08f);

    uint32 const buyout = std::max<uint32>(1, static_cast<uint32>(unitValue * count * depthFactor * noise));
    uint32 const startBid = std::max<uint32>(1, static_cast<uint32>(buyout * frand(0.55f, 0.85f)));
    uint32 const duration = AUCTION_DURATIONS[urand(0, 2)];

    AuctionEntry* auction = new AuctionEntry();
    auction->Id = sObjectMgr->GenerateAuctionID();
    auction->houseId = AuctionHouseId(ahEntry->houseId);
    auction->item_guid = item->GetGUID();
    auction->item_template = item->GetEntry();
    auction->itemCount = item->GetCount();
    auction->owner = bot->GetGUID();
    auction->startbid = startBid;
    auction->bidder = ObjectGuid::Empty;
    auction->bid = 0;
    auction->buyout = buyout;
    auction->expire_time = GameTime::GetGameTime().count() + duration;
    auction->deposit = 0;  // deliberate: bots do not pay a deposit, see DESIGN section 1.1
    auction->auctionHouseEntry = ahEntry;

    // Order mirrors WorldSession::HandleAuctionSellItem exactly: hand the item to the auction
    // manager, register the auction, remove the item from the bag, and only then write. Doing the
    // removal before the registration leaves a window where the item belongs to nobody.
    sAuctionMgr->AddAItem(item);
    house->AddAuction(auction);

    bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    item->DeleteFromInventoryDB(trans);
    item->SaveToDB(trans);
    auction->SaveToDB(trans);
    bot->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    uint32 attempts;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        ++_listingsByOwner[bot->GetGUID()];
        attempts = ++_listingAttempts[item->GetGUID().GetCounter()];
    }

    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbot_auction_attempts (item_guid, attempts) VALUES ({}, {}) "
        "ON DUPLICATE KEY UPDATE attempts = {}",
        item->GetGUID().GetCounter(), attempts, attempts);

    ++_stats.posted;
    _stats.goldListed += buyout;

    LOG_DEBUG("playerbots", "[Economy] {} listed {}x{} ({}) for {}c buyout (depth {}, factor {:.2f})",
              bot->GetName(), count, proto->Name1, proto->ItemId, buyout, depth, depthFactor);

    return true;
}

std::vector<BotEconomyMgr::Bargain> BotEconomyMgr::SampleBargains(TeamId team, uint32 limit) const
{
    std::vector<Bargain> picked;

    std::shared_ptr<BargainList const> snapshot = _bargains.load(std::memory_order_acquire);
    if (!snapshot || snapshot->empty() || !limit)
        return picked;

    AuctionHouseId const teamHouse = team == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde;
    AuctionHouseEntry const* ahEntry = AuctionHouseMgr::GetAuctionHouseEntryFromHouse(teamHouse);
    if (!ahEntry)
        return picked;

    AuctionHouseId const visible = AuctionHouseId(ahEntry->houseId);

    // Random offset rather than always scanning from the front: otherwise every bot in the realm
    // evaluates the same handful of listings and they all try to buy the same one.
    size_t const size = snapshot->size();
    size_t const start = urand(0, static_cast<uint32>(size - 1));

    picked.reserve(limit);
    for (size_t i = 0; i < size && picked.size() < limit; ++i)
    {
        Bargain const& bargain = (*snapshot)[(start + i) % size];
        if (bargain.houseId != visible)
            continue;

        picked.push_back(bargain);
    }

    return picked;
}

bool BotEconomyMgr::BuyoutAuction(Player* bot, uint32 auctionId, AuctionHouseId houseId)
{
    if (!bot)
        return false;

    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
    if (!house)
        return false;

    // Re-resolve: between the bargain being sampled and this running, another bot may have bought
    // it, or it may have expired.
    AuctionEntry* auction = house->GetAuction(auctionId);
    if (!auction || !auction->buyout)
        return false;

    // Buying your own listing would launder gold out of the realm and hide a posting bug behind a
    // plausible-looking sale.
    if (auction->owner == bot->GetGUID())
        return false;

    if (bot->GetMoney() < auction->buyout)
        return false;

    uint32 const buyout = auction->buyout;
    uint32 const itemId = auction->item_template;
    uint32 const itemCount = auction->itemCount;
    ObjectGuid const seller = auction->owner;
    uint32 const itemGuid2 = auction->item_guid.GetCounter();

    // Mirrors the buyout branch of WorldSession::HandleAuctionPlaceBid. The mails are what actually
    // pay the seller and deliver the goods, and they must be inside the transaction.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    bot->ModifyMoney(-int32(buyout));
    if (auction->bidder)
        sAuctionMgr->SendAuctionOutbiddedMail(auction, buyout, bot, trans);

    auction->bidder = bot->GetGUID();
    auction->bid = buyout;

    sAuctionMgr->SendAuctionSalePendingMail(auction, trans);
    sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
    sAuctionMgr->SendAuctionWonMail(auction, trans);
    sScriptMgr->OnAuctionSuccessful(house, auction);

    auction->DeleteFromDB(trans);

    sAuctionMgr->RemoveAItem(auction->item_guid);

    // RemoveAuction deletes the entry, so nothing may touch `auction` after this point.
    house->RemoveAuction(auction);

    bot->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    // The listing that just went away belonged to the seller, not the buyer; crediting the buyer
    // would let a bot free up its own posting slots by shopping.
    {
        std::lock_guard<std::mutex> guard(_mutex);
        auto itr = _listingsByOwner.find(seller);
        if (itr != _listingsByOwner.end() && itr->second)
            --itr->second;
    }

    {
        std::lock_guard<std::mutex> guard(_mutex);
        _listingAttempts.erase(itemGuid2);
    }
    PlayerbotsDatabase.Execute("DELETE FROM playerbot_auction_attempts WHERE item_guid = {}", itemGuid2);

    ++_stats.bought;
    _stats.goldSpent += buyout;

    LOG_DEBUG("playerbots", "[Economy] {} bought out {}x{} for {}c", bot->GetName(), itemCount, itemId, buyout);

    return true;
}

void BotEconomyMgr::PrintStats()
{
    uint32 indexed = 0;
    uint32 listings = 0;
    GetIndexSummary(indexed, listings);

    // Listing counts by quality, so an operator can see at a glance whether the house is full of
    // greens worth having or a wall of vendor-grade reagents.
    uint32 byQuality[ITEM_QUALITY_ARTIFACT + 1] = {};
    uint32 sellers = 0;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        sellers = static_cast<uint32>(_listingsByOwner.size());

        for (auto const& [itemId, record] : _records)
        {
            if (!record.depth)
                continue;

            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
                if (proto->Quality <= ITEM_QUALITY_ARTIFACT)
                    byQuality[proto->Quality] += record.depth;
        }
    }

    // The gold supply is the number that actually matters: posting and buying only move gold
    // between bots, but vendoring, quest rewards and mail cuts do not, so this is the inflation
    // signal.
    //
    // Summed over *online* bots from memory rather than from the DB. The obvious query joins
    // `characters` against `playerbots_random_bots`, but those live in two different databases
    // whose names are configurable, so a cross-database join would only work on a realm that
    // happens to be named the way this one is.
    uint64 botGold = 0;
    uint32 counted = 0;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!bot)
            continue;

        botGold += bot->GetMoney();
        ++counted;
    }

    static char const* const qualityNames[] = {"poor", "common", "uncommon", "rare", "epic",
                                               "legendary", "artifact"};

    LOG_INFO("playerbots", "=== Bot economy ===");
    LOG_INFO("playerbots", "Price index: {} items, {} live listings from {} sellers", indexed, listings, sellers);

    for (uint32 quality = 0; quality <= ITEM_QUALITY_ARTIFACT; ++quality)
        if (byQuality[quality])
            LOG_INFO("playerbots", "  {:>9}: {} listings", qualityNames[quality], byQuality[quality]);

    LOG_INFO("playerbots", "Posted: {} ({} failed, {} suppressed by depth)", _stats.posted.load(),
             _stats.postFailed.load(), _stats.suppressed.load());
    LOG_INFO("playerbots", "Bought: {}", _stats.bought.load());
    LOG_INFO("playerbots", "Gold listed: {}g   Gold spent at auction: {}g", _stats.goldListed.load() / 10000,
             _stats.goldSpent.load() / 10000);
    LOG_INFO("playerbots", "Gold held by {} online bots: {}g (avg {}g)", counted, botGold / 10000,
             counted ? (botGold / counted) / 10000 : 0);

    // Sell-through: what share of everything ever posted has actually cleared. A house where this
    // sits near zero is accumulating, not trading, and the buying side needs looking at.
    uint32 const posted = _stats.posted.load();
    if (posted)
        LOG_INFO("playerbots", "Sell-through: {:.1f}% of posted listings bought by bots",
                 100.0f * _stats.bought.load() / posted);
}
