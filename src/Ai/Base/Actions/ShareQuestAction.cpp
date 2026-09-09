/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ShareQuestAction.h"
#include "Event.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "QuestPackets.h"

bool ShareQuestAction::Execute(Event event)
{
    std::string const link = event.getParam();
    if (!GetMaster())
        return false;

    PlayerbotChatHandler handler(GetMaster());
    uint32 entry = handler.extractQuestId(link);
    if (!entry)
        return false;

    Quest const* quest = sObjectMgr->GetQuestTemplate(entry);
    if (!quest)
        return false;

    // remove all quest entries for 'entry' from quest log
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 logQuest = bot->GetQuestSlotQuestId(slot);
        if (logQuest == entry)
        {
            WorldPacket p(CMSG_PUSHQUESTTOPARTY);
            p << entry;
            WorldPackets::Quest::PushQuestToParty pushQuest(std::move(p));
            pushQuest.Read();
            bot->GetSession()->HandlePushQuestToParty(pushQuest);
            botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "quest_shared", "Quest shared", {}));
            return true;
        }
    }

    return false;
}

bool AutoShareQuestAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    bool shared = false;

    // Forget offers for quests this bot no longer carries, so the record cannot grow without bound
    // over a long session -- and so a quest that is abandoned and picked up again may be offered
    // once more, which is the behaviour a player would expect.
    std::set<uint32> inLog;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 const logQuest = bot->GetQuestSlotQuestId(slot))
            inLog.insert(logQuest);

    for (auto itr = _offered.begin(); itr != _offered.end();)
        itr = inLog.count(itr->first) ? std::next(itr) : _offered.erase(itr);

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const logQuest = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = sObjectMgr->GetQuestTemplate(logQuest);

        if (!quest)
            continue;

        // The core's own gate: the quest must be flagged sharable and the bot must actually hold it.
        // Checking it here as well means we do not record an offer for a push that would be dropped.
        if (!bot->CanShareQuest(logQuest))
            continue;

        // Who this push would genuinely reach. HandlePushQuestToParty offers to the whole party in
        // one call, so the eligibility test is repeated here to know which members to mark as
        // offered -- and to avoid pushing at all when nobody new would receive it.
        std::vector<ObjectGuid> newRecipients;

        for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
        {
            Player* player = ref->GetSource();

            if (!player || player == bot || !player->IsInWorld() || !botAI->IsSafe(player))
                continue;

            // Same map is the core's actual requirement -- there is no proximity rule. The previous
            // ten-yard limit was stricter than the game and defeated the point: a party that has
            // drifted apart is exactly when sharing is worth doing.
            if (!player->IsInMap(bot))
                continue;

            if (_offered.count({logQuest, player->GetGUID()}))
                continue;

            if (!player->SatisfyQuestStatus(quest, false))
                continue;

            if (player->GetQuestStatus(logQuest) == QUEST_STATUS_COMPLETE)
                continue;

            if (!player->CanTakeQuest(quest, false))
                continue;

            if (!player->SatisfyQuestLog(false))
                continue;

            // Mid-decision on some other offer. Skipped without being recorded, so it is retried
            // rather than silently losing its one chance at this quest.
            if (player->GetDivider())
                continue;

            newRecipients.push_back(player->GetGUID());
        }

        if (newRecipients.empty())
            continue;

        WorldPacket p(CMSG_PUSHQUESTTOPARTY);
        p << logQuest;
        WorldPackets::Quest::PushQuestToParty pushQuest(std::move(p));
        pushQuest.Read();
        bot->GetSession()->HandlePushQuestToParty(pushQuest);

        // Recorded whether or not they accept. Acceptance is visible next pass through the
        // eligibility checks; refusal is not visible at all, which is the case this exists for.
        for (ObjectGuid const& guid : newRecipients)
            _offered.insert({logQuest, guid});

        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_shared", "Quest shared", {}));
        shared = true;
    }

    return shared;
}

bool AutoShareQuestAction::isUseful()
{
    // Being in a party is the whole requirement.
    //
    // This used to also demand that the bot had no human master, which silently disabled sharing
    // for exactly the party the player is in -- alt bots grouped with their owner never shared a
    // quest with anyone, including each other.
    return bot->GetGroup() != nullptr;
}
