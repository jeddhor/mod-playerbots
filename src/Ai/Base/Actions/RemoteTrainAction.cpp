/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RemoteTrainAction.h"

#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "BotTrainingMgr.h"


bool RemoteTrainAction::isUseful()
{
    return sPlayerbotAIConfig.remoteTrainingEnabled && bot->GetMoney() > 0;
}

bool RemoteTrainAction::Execute(Event /*event*/)
{
    // One implementation, in BotTrainingMgr. This action stays so that anything already asking for
    // "remote train" keeps working, but the manager is what actually decides and spends -- having
    // the logic in two places is how the two paths drift apart.
    return sBotTrainingMgr.TrainNow(bot) > 0;
}
