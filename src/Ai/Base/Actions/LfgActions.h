/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGACTIONS_H
#define PLAYERBOTS_LFGACTIONS_H

#include "InventoryAction.h"

class Player;
class PlayerbotAI;

/**
 * The single role this character's spec is built for.
 *
 * Free functions rather than members because BotLfgMgr needs the same answer for a bot it is about
 * to queue on a person's behalf, and it has no action object to ask. Two copies of this table
 * would drift, and the one that drifted would be the one deciding whether a group can form.
 */
uint32 LfgPrimaryRoleFor(Player* bot);

/// Every role this character can actually fill, not just the one its spec is built for.
uint32 LfgRolesFor(Player* bot);

class LfgJoinAction : public InventoryAction
{
public:
    LfgJoinAction(PlayerbotAI* botAI, std::string const name = "lfg join") : InventoryAction(botAI, name) {}

    bool Execute(Event event) override;
    bool isUseful() override;

protected:
    bool JoinLFG();
    uint32 GetRoles();

    /// The single role this bot's spec is built for, before hybrid capabilities are added.
    uint32 GetPrimaryRole();
};

class LfgAcceptAction : public LfgJoinAction
{
public:
    LfgAcceptAction(PlayerbotAI* botAI) : LfgJoinAction(botAI, "lfg accept") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

class LfgRoleCheckAction : public LfgJoinAction
{
public:
    LfgRoleCheckAction(PlayerbotAI* botAI) : LfgJoinAction(botAI, "lfg role check") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

class LfgLeaveAction : public Action
{
public:
    LfgLeaveAction(PlayerbotAI* botAI) : Action(botAI, "lfg leave") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class LfgTeleportAction : public Action
{
public:
    LfgTeleportAction(PlayerbotAI* botAI) : Action(botAI, "lfg teleport") {}

    bool Execute(Event event) override;
};

#endif
