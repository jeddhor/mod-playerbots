/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaveGroupAction.h"
#include "Map.h"
#include "LFGMgr.h"
#include "Event.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"

bool LeaveGroupAction::Execute(Event event)
{
    Player* player = event.getOwner();
    if (player == botAI->GetMaster())
        return Leave();

    return false;
}

bool PartyCommandAction::Execute(Event event)
{
    WorldPacket& p = event.getPacket();
    p.rpos(0);
    uint32 operation;
    std::string member;

    p >> operation >> member;

    if (operation != PARTY_OP_LEAVE)
        return false;
    // Only leave if master has left the party, and randombot cannot set new master.
    Player* master = GetMaster();
    if (master && member == master->GetName())
    {
        if (sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            Player* newMaster = botAI->FindNewMaster();
            if (newMaster || bot->InBattleground())
            {
                botAI->SetMaster(newMaster);
                return false;
            }
        }
        return Leave();
    }
    return false;
}

bool UninviteAction::Execute(Event event)
{
    WorldPacket& p = event.getPacket();
    if (p.GetOpcode() == CMSG_GROUP_UNINVITE)
    {
        p.rpos(0);
        std::string memberName;
        p >> memberName;

        // player not found
        if (!normalizePlayerName(memberName))
        {
            return false;
        }

        if (bot->GetName() == memberName)
            return Leave();
    }

    if (p.GetOpcode() == CMSG_GROUP_UNINVITE_GUID)
    {
        p.rpos(0);
        ObjectGuid guid;
        p >> guid;

        if (bot->GetGUID() == guid)
            return Leave();
    }

    return false;
}

bool LeaveGroupAction::Leave()
{
    if (!botAI)
        return false;

    Player* master = botAI -> GetMaster();
    if (master)
        botAI->TellMaster(
            PlayerbotTextMgr::instance().GetBotTextOrDefault("goodbye", "Goodbye!", {}),
            PLAYERBOT_SECURITY_TALK);

    botAI->LeaveOrDisbandGroup();
    return true;
}

bool LeaveFarAwayAction::Execute(Event /*event*/)
{
    // allow bot to leave party when they want
    return Leave();
}

bool LeaveFarAwayAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    if (bot->InBattlegroundQueue())
        return false;

    if (!bot->GetGroup())
        return false;

    // Never walk out of a dungeon group.
    //
    // LFG spends real effort assembling five compatible players across roles and level brackets, and
    // a bot whose GrouperType is SOLO would otherwise leave the moment it landed -- stranding every
    // member alone in its own instance. That is exactly what happened: nineteen bots entered
    // dungeons via LFG and ended up ungrouped, one per instance, until ten instances were live
    // against a cap of five. The cap counts instances and was working correctly; the groups were
    // dissolving underneath it.
    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        return false;

    if (Map* map = bot->FindMap(); map && map->Instanceable())
        return false;

    // A self bot is the player's own character, and its party membership is the player's decision
    // rather than the AI's. This matters most when the self bot leads: the guard below reads
    // "a leader does not leave" but exempts self bots, so a self-bot leader fell through to the
    // GrouperType::SOLO case and disbanded the party the player had just built around them.
    if (IsSelfBot(bot))
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Player* trueMaster = botAI->GetMaster();
    if (!groupLeader || (bot == groupLeader && !IsSelfBot(bot)))
        return false;

    PlayerbotAI* groupLeaderBotAI = nullptr;
    if (groupLeader)
        groupLeaderBotAI = GET_PLAYERBOT_AI(groupLeader);
    if (groupLeader && !groupLeaderBotAI)
        return false;

    if (trueMaster && !GET_PLAYERBOT_AI(trueMaster))
        return false;

    if (botAI->IsAltBot() &&
        (!groupLeaderBotAI || IsSelfBot(groupLeader)))  // Don't leave when an altbot is grouped under a regular real player or a selfbot.
        return false;

    if (botAI->GetGrouperType() == GrouperType::SOLO)
        return true;

    uint32 dCount = AI_VALUE(uint32, "death count");

    if (dCount > 9)
        return true;

    if (dCount > 4 && !botAI->HasGameClientMaster())
        return true;

    if (bot->GetGuildId() == groupLeader->GetGuildId())
    {
        if (bot->GetLevel() > groupLeader->GetLevel() + 5)
        {
            if (AI_VALUE(bool, "should get money"))
                return false;
        }
    }

    if (abs(int32(groupLeader->GetLevel() - bot->GetLevel())) > 4)
        return true;

    if (bot->GetMapId() != groupLeader->GetMapId() || bot->GetDistance2d(groupLeader) >= 2 * sPlayerbotAIConfig.rpgDistance)
    {
        return true;
    }

    return false;
}
