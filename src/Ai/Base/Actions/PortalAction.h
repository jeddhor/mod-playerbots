/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PORTALACTION_H
#define PLAYERBOTS_PORTALACTION_H

#include "Action.h"

#include <string>
#include <vector>

class Player;
class PlayerbotAI;

/**
 * R28 -- a mage opens a portal because somebody asked it to.
 *
 * The first feature where a bot provides a service on request rather than pursuing its own agenda,
 * and the whole interaction is chat: ask what it knows, it answers; name a city, it casts.
 *
 * The mage behaves like a mage. It needs the reagent, it needs to be somewhere portals are allowed,
 * and it refuses out loud when it cannot -- "I can't cast that here" is a better answer than
 * silence, and a bot that teleported you directly would not feel like asking a mage at all.
 */
class PortalAction : public Action
{
public:
    PortalAction(PlayerbotAI* botAI) : Action(botAI, "portal") {}

    bool Execute(Event event) override;

private:
    struct KnownPortal
    {
        uint32 spellId;
        std::string destination;  //< the city, lowercased, as it appears after "Portal: "
    };

    /// Portals this bot actually knows, read from its spell map rather than from a table of
    /// assumptions about what a mage of a given level should have learned.
    std::vector<KnownPortal> KnownPortals() const;

    void Refuse(Player* requester, std::string const& why) const;
};

#endif
