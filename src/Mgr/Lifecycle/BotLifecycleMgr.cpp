/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotLifecycleMgr.h"

#include "AccountMgr.h"
#include "AuctionHouseMgr.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Field.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"
#include "Random.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"

#include <sstream>
#include <vector>

namespace
{
/// Houses a bot can have listed in. Neutral included: a goblin auctioneer takes anyone's goods.
constexpr AuctionHouseId AUCTION_HOUSES[] = {AuctionHouseId::Alliance, AuctionHouseId::Horde,
                                             AuctionHouseId::Neutral};
}  // namespace

void BotLifecycleMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.retireEnabled || !sPlayerbotAIConfig.randomBotAccounts.size())
        return;

    if (_timer > diff)
    {
        _timer -= diff;
        return;
    }

    _timer = sPlayerbotAIConfig.retireIntervalSeconds * IN_MILLISECONDS;

    SweepOrphanRows();

    // Offline candidates only. A logged-in bot is being updated by the world at the same moment, and
    // deleting the character underneath it is a crash looking for somewhere to happen. There is no
    // hurry: it will be offline within the hour.
    std::ostringstream accounts;
    for (uint32 accountId : sPlayerbotAIConfig.randomBotAccounts)
    {
        if (accounts.tellp())
            accounts << ',';
        accounts << accountId;
    }

    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, account, name, level, totaltime FROM characters "
        "WHERE account IN ({}) AND online = 0 AND deleteInfos_Account IS NULL "
        "AND level >= {} AND totaltime >= {} ORDER BY RAND() LIMIT {}",
        accounts.str(), uint32(sPlayerbotAIConfig.retireMinLevel), sPlayerbotAIConfig.retireMinTimePlayed,
        sPlayerbotAIConfig.retirePerInterval);

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        Retire(fields[0].Get<uint32>(), fields[1].Get<uint32>(), fields[2].Get<std::string>(), fields[3].Get<uint8>(),
               fields[4].Get<uint32>());
    } while (result->NextRow());
}

uint32 BotLifecycleMgr::SettleAuctions(ObjectGuid guid)
{
    uint32 cancelled = 0;
    uint32 refunded = 0;

    for (AuctionHouseId houseId : AUCTION_HOUSES)
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
        if (!house)
            continue;

        // Collected first: cancelling walks the same map the iteration is over.
        std::vector<AuctionEntry*> owned;
        for (auto itr = house->GetAuctionsBegin(); itr != house->GetAuctionsEnd(); ++itr)
            if (itr->second && itr->second->owner == guid)
                owned.push_back(itr->second);

        for (AuctionEntry* auction : owned)
        {
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

            // The bid is another bot's money and has already left its pocket. Refunding it through the
            // core's own path is what keeps realm gold whole across a retirement -- and it is the one
            // step that cannot be recovered afterwards if it is skipped.
            if (auction->bidder)
            {
                sAuctionMgr->SendAuctionCancelledToBidderMail(auction, trans);
                ++refunded;
            }

            // The item goes with its owner. Returning it by mail would post to a character that is
            // about to stop existing, which is exactly the orphan this is meant to avoid.
            sAuctionMgr->RemoveAItem(auction->item_guid, true, &trans);

            auction->DeleteFromDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            house->RemoveAuction(auction);
            ++cancelled;
        }
    }

    if (cancelled || refunded)
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _auctionsCancelled += cancelled;
        _bidsRefunded += refunded;
    }

    return cancelled;
}

void BotLifecycleMgr::SweepOrphanRows()
{
    // RandomPlayerbotMgr keeps writing periodic "update" and "logout" events for every guid in its
    // roster, and a retiree stays in that roster until the manager next rebuilds it -- so a row can
    // reappear moments after it was deleted. Sweeping by "has no character any more" catches that,
    // and anything an earlier retirement left behind.
    PlayerbotsDatabase.Execute("DELETE FROM playerbots_random_bots WHERE bot > 0 AND bot NOT IN "
                               "(SELECT guid FROM {}.characters)",
                               CharacterDatabase.GetConnectionInfo()->database);
}

void BotLifecycleMgr::ForgetBot(ObjectGuid::LowType guid)
{
    // The module's own per-bot state. Left behind it would be read back by whatever character later
    // happens to be given the same guid.
    PlayerbotsDatabase.Execute("DELETE FROM playerbots_random_bots WHERE bot = {} OR owner = {}", guid, guid);
    PlayerbotsDatabase.Execute("DELETE FROM playerbots_db_store WHERE guid = {}", guid);
    PlayerbotsDatabase.Execute("DELETE FROM playerbots_guild_tasks WHERE owner = {} OR guildid = {}", guid, guid);
    PlayerbotsDatabase.Execute("DELETE FROM playerbot_profile WHERE guid = {}", guid);
    PlayerbotsDatabase.Execute("DELETE FROM playerbot_goal WHERE guid = {}", guid);
    PlayerbotsDatabase.Execute("DELETE FROM playerbot_craft_goal WHERE guid = {}", guid);

    SweepOrphanRows();
}

bool BotLifecycleMgr::CreateReplacement(uint32 accountId, std::string& createdName)
{
    // A class the account can actually roll, chosen first so the race can be picked to match it.
    std::vector<uint8> classes;
    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        if (!((1 << (cls - 1)) & CLASSMASK_ALL_PLAYABLE) || !sChrClassesStore.LookupEntry(cls))
            continue;

        if ((1 << (cls - 1)) & sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_CLASSMASK))
            continue;

        classes.push_back(cls);
    }

    if (classes.empty())
        return false;

    WorldSession* session = new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER,
                                             EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0, false, false,
                                             0, true);

    // An empty cache makes the factory generate a name rather than draw from a prepared pool.
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> nameCache;
    RandomPlayerbotFactory factory;

    Player* replacement = factory.CreateRandomBot(session, classes[urand(0, classes.size() - 1)], nameCache);
    if (!replacement)
    {
        delete session;
        return false;
    }

    createdName = replacement->GetName();

    replacement->SaveToDB(true, false);
    sCharacterCache->AddCharacterCacheEntry(replacement->GetGUID(), accountId, replacement->GetName(),
                                            replacement->getGender(), replacement->getRace(),
                                            replacement->getClass(), replacement->GetLevel());
    replacement->CleanupsBeforeDelete();
    delete replacement;
    delete session;

    std::unique_lock<std::shared_mutex> guard(_mutex);
    ++_replacementsCreated;
    return true;
}

bool BotLifecycleMgr::Retire(ObjectGuid::LowType guid, uint32 accountId, std::string const& name, uint8 level,
                             uint32 played)
{
    ObjectGuid const playerGuid = ObjectGuid::Create<HighGuid::Player>(guid);

    // Logged in since the query: leave it for the next pass.
    if (ObjectAccessor::FindConnectedPlayer(playerGuid))
        return false;

    uint32 const cancelled = SettleAuctions(playerGuid);

    // Out of the roster before the character goes, or the manager writes fresh event rows for a guid
    // that no longer exists -- which is exactly what the sweep kept finding.
    sRandomPlayerbotMgr.ForgetRetiredBot(guid);

    // Everything else the character owns -- mail, items, guild membership, arena teams, friends lists
    // -- is the core's own business and it does all of it. Only the auctions above are outside its
    // knowledge.
    Player::DeleteFromDB(guid, accountId, true, true);
    ForgetBot(guid);

    std::string replacementName;
    bool const replaced = CreateReplacement(accountId, replacementName);

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_retired;
    }

    LOG_INFO("playerbots", "[Lifecycle] retired {} (level {}, {} days played, {} listings cancelled); {}", name,
             uint32(level), played / DAY, cancelled,
             replaced ? Acore::StringFormat("{} takes the account", replacementName)
                      : "no replacement could be created");

    return true;
}

std::string BotLifecycleMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("lifecycle: {} retired, {} replacements created, {} listings cancelled, {} bids refunded",
                               _retired, _replacementsCreated, _auctionsCancelled, _bidsRefunded);
}
