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

    LOG_INFO("server.loading", ">> Loaded {} playerbot quest blacklist entries in {} ms", _records.size(),
             GetMSTimeDiffToNow(oldMSTime));
}

bool QuestBlacklistMgr::IsBlacklisted(uint32 questId)
{
    if (!questId)
        return false;

    std::shared_lock<std::shared_mutex> guard(_mutex);

    auto itr = _records.find(questId);
    if (itr == _records.end())
        return false;

    // Any recorded completion clears the verdict: if some bot managed it, the quest is workable
    // and whatever went wrong for the failing bots was situational.
    if (itr->second.successCount)
        return false;

    return itr->second.failCount >= sPlayerbotAIConfig.questBlacklistFailThreshold;
}

void QuestBlacklistMgr::ReportFailure(uint32 questId)
{
    if (!questId)
        return;

    uint32 failCount;
    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        failCount = ++_records[questId].failCount;
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
    }

    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbot_quest_blacklist (quest_id, success_count) VALUES ({}, {}) "
        "ON DUPLICATE KEY UPDATE success_count = {}",
        questId, successCount, successCount);
}
