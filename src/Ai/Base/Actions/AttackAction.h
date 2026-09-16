/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ATTACKACTION_H
#define PLAYERBOTS_ATTACKACTION_H

#include "MovementActions.h"

class PlayerbotAI;

class AttackAction : public MovementAction
{
public:
    AttackAction(PlayerbotAI* botAI, std::string const name) : MovementAction(botAI, name) {}

    bool Execute(Event event) override;

protected:
    bool Attack(Unit* target, bool with_pet = true);

private:
    // When a self bot last started an attack it could not land, and on whom. See Attack().
    ObjectGuid _outOfReachAttackTarget;
    uint32 _outOfReachAttackMs{0};
};

class AttackMyTargetAction : public AttackAction
{
public:
    AttackMyTargetAction(PlayerbotAI* botAI, std::string const name = "attack my target") : AttackAction(botAI, name) {}

    bool Execute(Event event) override;
};

class AttackDuelOpponentAction : public AttackAction
{
public:
    AttackDuelOpponentAction(PlayerbotAI* botAI, std::string const name = "attack duel opponent")
        : AttackAction(botAI, name)
    {
    }

public:
    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
