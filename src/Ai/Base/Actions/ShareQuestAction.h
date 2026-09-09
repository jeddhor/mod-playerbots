/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHAREQUESTACTION_H
#define PLAYERBOTS_SHAREQUESTACTION_H

#include "Action.h"
#include "ObjectGuid.h"

#include <set>
#include <utility>

class PlayerbotAI;

class ShareQuestAction : public Action
{
public:
    ShareQuestAction(PlayerbotAI* botAI, std::string name = "share quest") : Action(botAI, name) { }
    bool Execute(Event event) override;
};

class AutoShareQuestAction : public ShareQuestAction
{
public:
    AutoShareQuestAction(PlayerbotAI* botAI) : ShareQuestAction(botAI, "auto share quest") {}
    bool Execute(Event event) override;

    bool isUseful() override;

private:
    /**
     * Who has already been offered which quest, so nobody is asked twice.
     *
     * A declined share is not recorded anywhere the server can see -- the quest simply does not
     * appear in the other player's log -- so without this the next pass finds them eligible again
     * and re-offers it, forever. For a bot that is merely noise; for the human in the party it is a
     * dialog box that will not stop coming back, which is the behaviour this has to avoid.
     *
     * An instance member rather than anything global: the engine builds one action per bot, so this
     * is naturally per-bot and never shared across map threads.
     */
    std::set<std::pair<uint32, ObjectGuid>> _offered;
};

#endif
