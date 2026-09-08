/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REMOTETRAINACTION_H
#define PLAYERBOTS_REMOTETRAINACTION_H

#include "Action.h"

class PlayerbotAI;

/**
 * Learns trainable spells and profession ranks without walking to a trainer.
 *
 * The same sanctioned abstraction as the auction house and the vendor: the travel is skipped, the
 * economics are not. The bot pays the trainer's full price, meets the level, skill and prerequisite
 * requirements, and respects the profession cap.
 *
 * The gold requirement is the point of the feature rather than a limitation on it. It couples
 * training to the economy -- a bot that cannot afford Journeyman stays Apprentice -- which is what
 * gives the auction house something to be *for*. Without training, every bot stays at its starting
 * ranks and the market never sees anything above tier-one materials.
 *
 * The existing RPG_TRAIN activity, which walks to a real trainer, is left in place: it is better
 * behaviour when the bot is near one, and this is the fallback for the great majority of the time
 * when it is not.
 */
class RemoteTrainAction : public Action
{
public:
    RemoteTrainAction(PlayerbotAI* botAI, std::string const name = "remote train") : Action(botAI, name) {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
