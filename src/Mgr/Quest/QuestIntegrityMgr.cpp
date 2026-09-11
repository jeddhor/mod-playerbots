/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestIntegrityMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"

#include <vector>

namespace
{
/**
 * Does `status` satisfy every counted objective `quest` declares?
 *
 * This deliberately does not call Player::CanCompleteQuest, for two reasons:
 *
 *   1. That function only evaluates objectives when the stored status is already INCOMPLETE. Handed
 *      a quest stored as COMPLETE -- which is every quest we are here to check -- it falls through
 *      to `return false`, so it would condemn all of them, sound records included.
 *   2. Its other exit conditions are not evidence of the corruption we are repairing. A completed
 *      timed quest has had its timer cleared, and a player may legitimately have spent gold a quest
 *      once required. Treating either as proof of a bad record would demote honest quests.
 *
 * So we check exactly the counters that the bad writes left at zero, and nothing else. Anything
 * this function is unsure about it reports as satisfied, leaving the record alone.
 */
bool ObjectivesSatisfied(Quest const* quest, QuestStatusData const& status)
{
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_DELIVER))
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemCount[i] && status.ItemCount[i] < quest->RequiredItemCount[i])
                return false;

    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_KILL | QUEST_SPECIAL_FLAGS_CAST | QUEST_SPECIAL_FLAGS_SPEAKTO))
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] && quest->RequiredNpcOrGoCount[i] &&
                status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
                return false;

    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_PLAYER_KILL))
        if (quest->GetPlayersSlain() && status.PlayerCount < quest->GetPlayersSlain())
            return false;

    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) && !status.Explored)
        return false;

    return true;
}
}  // namespace

uint32 QuestIntegrityMgr::RepairStoredCompletions(Player* player)
{
    if (!player)
        return 0;

    // Collected first, applied after: IncompleteQuest touches zone and area auras, which is more
    // than we want happening under an iterator over the map it also writes to.
    std::vector<uint32> broken;

    for (auto const& [questId, status] : player->getQuestStatusMap())
    {
        if (status.Status != QUEST_STATUS_COMPLETE)
            continue;

        // Already handed in; whatever this record says, it is not driving a turn-in.
        if (player->GetQuestRewardStatus(questId))
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        // Quests that complete on acceptance have no counters to fall short, so a stored completion
        // is the expected state rather than evidence of anything.
        if (quest->IsAutoComplete() || !quest->GetQuestMethod())
            continue;

        if (ObjectivesSatisfied(quest, status))
            continue;

        broken.push_back(questId);
    }

    for (uint32 questId : broken)
    {
        player->IncompleteQuest(questId);
        LOG_INFO("playerbots", "[QuestIntegrity] {}: quest {} ({}) was stored complete with objectives unmet; "
                               "reset to incomplete",
                 player->GetName(), questId, sObjectMgr->GetQuestTemplate(questId)->GetTitle());
    }

    return uint32(broken.size());
}
