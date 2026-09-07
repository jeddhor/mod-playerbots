/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotHelpMgr.h"

#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "StringFormat.h"

#include <algorithm>

void BotHelpMgr::RaiseRequest(Player* caller, uint32 questId)
{
    if (!caller || !questId || !sPlayerbotAIConfig.helpEnabled)
        return;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return;

    uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

    std::unique_lock<std::shared_mutex> lock(_mutex);

    // One outstanding request per bot. A bot failing three quests should not be able to summon
    // three separate rescue parties.
    for (Request& existing : _requests)
    {
        if (existing.caller == caller->GetGUID())
        {
            existing.questId = questId;
            existing.expiresAt = now + sPlayerbotAIConfig.helpRequestSeconds;
            existing.x = caller->GetPositionX();
            existing.y = caller->GetPositionY();
            existing.z = caller->GetPositionZ();
            existing.mapId = caller->GetMapId();
            return;
        }
    }

    Request request;
    request.caller = caller->GetGUID();
    request.questId = questId;
    request.questLevel = quest->GetQuestLevel() > 0 ? static_cast<uint32>(quest->GetQuestLevel()) : caller->GetLevel();
    request.mapId = caller->GetMapId();
    request.x = caller->GetPositionX();
    request.y = caller->GetPositionY();
    request.z = caller->GetPositionZ();
    request.callerLevel = caller->GetLevel();
    request.expiresAt = now + sPlayerbotAIConfig.helpRequestSeconds;

    _requests.push_back(request);
    ++_raised;

    LOG_DEBUG("playerbots", "[Help] {} (level {}) asked for help with quest {} (level {}) on map {}",
              caller->GetName(), request.callerLevel, questId, request.questLevel, request.mapId);
}

bool BotHelpMgr::FindRequestFor(Player* responder, Request& out)
{
    if (!responder || !sPlayerbotAIConfig.helpEnabled)
        return false;

    uint32 const level = responder->GetLevel();

    std::shared_lock<std::shared_mutex> lock(_mutex);

    for (Request const& request : _requests)
    {
        if (request.caller == responder->GetGUID())
            continue;

        if (request.responders >= sPlayerbotAIConfig.helpMaxResponders)
            continue;

        // Same map only. Crossing continents to answer a call would have a bot spend an hour
        // travelling to a fight that ended long ago.
        if (request.mapId != responder->GetMapId())
            continue;

        // The fairness rule, anchored to the quest rather than the caller: a responder must be
        // capable of surviving the content without trivialising it.
        if (level + sPlayerbotAIConfig.helpMaxLevelsBelow < request.callerLevel)
            continue;

        if (level > request.questLevel + sPlayerbotAIConfig.helpMaxLevelsAbove)
            continue;

        out = request;
        return true;
    }

    return false;
}

void BotHelpMgr::AcceptRequest(ObjectGuid caller, ObjectGuid responder)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);

    for (Request& request : _requests)
    {
        if (request.caller != caller)
            continue;

        ++request.responders;
        ++_answered;

        LOG_DEBUG("playerbots", "[Help] responder {} heading to {} ({} of {})", responder.GetCounter(),
                  caller.GetCounter(), request.responders, sPlayerbotAIConfig.helpMaxResponders);
        return;
    }
}

void BotHelpMgr::ClearRequest(ObjectGuid caller)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _requests.erase(std::remove_if(_requests.begin(), _requests.end(),
                                   [caller](Request const& r) { return r.caller == caller; }),
                    _requests.end());
}

void BotHelpMgr::Update(uint32 diff)
{
    _timer += diff;
    if (_timer < 10 * IN_MILLISECONDS)
        return;

    _timer = 0;

    uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

    std::unique_lock<std::shared_mutex> lock(_mutex);
    _requests.erase(std::remove_if(_requests.begin(), _requests.end(),
                                   [now](Request const& r) { return r.expiresAt <= now; }),
                    _requests.end());
}

std::string BotHelpMgr::DescribeRequests() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);

    std::string out = Acore::StringFormat("Help requests: {} open, {} raised, {} answered\n", _requests.size(),
                                          _raised, _answered);
    for (Request const& r : _requests)
        out += Acore::StringFormat("  caller {} lvl {} quest {} (lvl {}) map {} responders {}\n",
                                   r.caller.GetCounter(), r.callerLevel, r.questId, r.questLevel, r.mapId,
                                   r.responders);

    return out;
}
