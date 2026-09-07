/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestBlacklistMgr.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"
#include "Timer.h"

#include <cstdlib>

void QuestBlacklistMgr::LoadQuestChains()
{
    if (QueryResult result = WorldDatabase.Query(
            "SELECT PrevQuestID, COUNT(*) FROM quest_template_addon WHERE PrevQuestID <> 0 GROUP BY PrevQuestID"))
    {
        do
        {
            Field* fields = result->Fetch();

            // PrevQuestID is signed in the schema: a negative value means "this quest must be in the
            // log", not "completed". Either way the referenced quest is a chain link, so take the
            // magnitude.
            int32 const prev = fields[0].Get<int32>();
            _unlockCounts[static_cast<uint32>(std::abs(prev))] += fields[1].Get<uint32>();
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", ">> Indexed {} quests that unlock further quests", _unlockCounts.size());
}

uint32 QuestBlacklistMgr::GetUnlockCount(uint32 questId) const
{
    auto itr = _unlockCounts.find(questId);
    return itr != _unlockCounts.end() ? itr->second : 0;
}

void QuestBlacklistMgr::LoadEscortQuests()
{
    // Escorts started by a C++ npc_escortAI creature: the quest giver has a scripted waypoint path.
    if (QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT cq.quest FROM creature_queststarter cq "
            "WHERE cq.id IN (SELECT DISTINCT entry FROM script_waypoint)"))
    {
        do
        {
            _escortQuests.insert(result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());
    }

    // Escorts driven by SmartAI: event 19 is SMART_EVENT_QUEST_ACCEPTED, action 53 starts a
    // waypoint path, so the pair means "accepting this quest sends an NPC walking".
    if (QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT event_param1 FROM smart_scripts "
            "WHERE event_type = 19 AND action_type = 53 AND event_param1 > 0"))
    {
        do
        {
            _escortQuests.insert(result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", ">> Identified {} escort quests bots will not attempt", _escortQuests.size());
}

bool QuestBlacklistMgr::IsEscortQuest(uint32 questId) const
{
    return _escortQuests.find(questId) != _escortQuests.end();
}

void QuestBlacklistMgr::Load()
{
    uint32 oldMSTime = getMSTime();

    std::unique_lock<std::shared_mutex> guard(_mutex);
    _records.clear();

    QueryResult result = PlayerbotsDatabase.Query("SELECT quest_id, fail_count, success_count FROM playerbot_quest_blacklist");
    if (!result)
    {
        LOG_INFO("server.loading", ">> Loaded 0 playerbot quest blacklist entries");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        uint32 questId = fields[0].Get<uint32>();

        Record& record = _records[questId];
        record.failCount = fields[1].Get<uint32>();
        record.successCount = fields[2].Get<uint32>();
    } while (result->NextRow());

    RebuildBlacklistSet();

    LOG_INFO("server.loading", ">> Loaded {} playerbot quest blacklist entries in {} ms", _records.size(),
             GetMSTimeDiffToNow(oldMSTime));

    LoadEscortQuests();
    LoadQuestChains();
}

void QuestBlacklistMgr::RebuildBlacklistSet()
{
    auto rebuilt = std::make_shared<BlacklistSet>();
    uint32 const threshold = sPlayerbotAIConfig.questBlacklistFailThreshold;

    for (auto const& entry : _records)
    {
        // Any recorded completion clears the verdict, exactly as the old read-time check did.
        if (!entry.second.successCount && entry.second.failCount >= threshold)
            rebuilt->insert(entry.first);
    }

    _blacklisted.store(std::move(rebuilt), std::memory_order_release);
}

bool QuestBlacklistMgr::IsBlacklisted(uint32 questId)
{
    if (!questId)
        return false;

    // Lock-free: one atomic load and one hash lookup. See the comment on `_blacklisted` for why
    // this cannot be a shared_lock - it is called 25 times per bot per idle tick from two places.
    std::shared_ptr<BlacklistSet const> snapshot = _blacklisted.load(std::memory_order_acquire);
    return snapshot && snapshot->count(questId) != 0;
}

void QuestBlacklistMgr::ReportFailure(uint32 questId)
{
    if (!questId)
        return;

    uint32 failCount;
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        failCount = ++_records[questId].failCount;
        RebuildBlacklistSet();
    }

    // All values are integers, so there is no injection surface here; the playerbots pool has no
    // prepared statement for this module-owned table.
    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbot_quest_blacklist (quest_id, fail_count) VALUES ({}, {}) "
        "ON DUPLICATE KEY UPDATE fail_count = {}",
        questId, failCount, failCount);
}

void QuestBlacklistMgr::ReportSuccess(uint32 questId)
{
    if (!questId)
        return;

    uint32 successCount;
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        successCount = ++_records[questId].successCount;
        RebuildBlacklistSet();
    }

    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbot_quest_blacklist (quest_id, success_count) VALUES ({}, {}) "
        "ON DUPLICATE KEY UPDATE success_count = {}",
        questId, successCount, successCount);
}
