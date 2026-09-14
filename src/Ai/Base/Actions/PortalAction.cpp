/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PortalAction.h"

#include "Event.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace
{
//: Portal spells are named "Portal: Stormwind" and so on, in every locale the client ships. Matching
//: the prefix is what lets this find portals the bot knows without a hardcoded list of spell ids --
//: which would go stale the moment a patch or a module adds a destination.
constexpr char const* PORTAL_PREFIX = "Portal: ";

std::string Lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}
}  // namespace

std::vector<PortalAction::KnownPortal> PortalAction::KnownPortals() const
{
    std::vector<KnownPortal> known;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;

        char const* name = info->SpellName[sWorld->GetDefaultDbcLocale()];
        if (!name || !*name)
            name = info->SpellName[LOCALE_enUS];
        if (!name || !*name)
            continue;

        std::string const full(name);
        if (full.rfind(PORTAL_PREFIX, 0) != 0)
            continue;

        known.push_back({spellId, Lowered(full.substr(std::strlen(PORTAL_PREFIX)))});
    }

    std::sort(known.begin(), known.end(),
              [](KnownPortal const& a, KnownPortal const& b) { return a.destination < b.destination; });
    return known;
}

void PortalAction::Refuse(Player* requester, std::string const& why) const
{
    if (requester)
        bot->Whisper(why, LANG_UNIVERSAL, requester);
}

bool PortalAction::Execute(Event event)
{
    if (!bot)
        return false;

    Player* requester = nullptr;
    if (Unit* owner = event.getOwner())
        requester = owner->ToPlayer();
    if (!requester)
        requester = GetMaster();
    if (!requester)
        return false;

    std::vector<KnownPortal> const known = KnownPortals();

    if (known.empty())
    {
        // Covers both "not a mage" and "a mage too low to have learned one", without having to ask
        // which: either way the honest answer is the same.
        Refuse(requester, "I don't know any portals.");
        return true;
    }

    std::string const wanted = Lowered(event.getParam());

    // No destination named: answer with what this bot can actually reach.
    if (wanted.empty() || wanted == "list")
    {
        std::string reply = "I can open portals to: ";
        for (size_t i = 0; i < known.size(); ++i)
            reply += (i ? ", " : "") + known[i].destination;

        bot->Whisper(reply, LANG_UNIVERSAL, requester);
        return true;
    }

    auto match = std::find_if(known.begin(), known.end(), [&wanted](KnownPortal const& portal) {
        // Prefix rather than equality, so "iron" finds Ironforge and nobody has to type a city name
        // exactly as the client spells it.
        return portal.destination.rfind(wanted, 0) == 0;
    });

    if (match == known.end())
    {
        Refuse(requester, "I can't open a portal to " + event.getParam() + ".");
        return true;
    }

    // The game's own rule, reported rather than silently ignored. A portal is for the group.
    if (!bot->GetGroup() || !bot->GetGroup()->IsMember(requester->GetGUID()))
    {
        Refuse(requester, "Invite me to your group and I'll open one.");
        return true;
    }

    // Reagents are deliberately not waived here, unlike food and vendors. Those abstractions exist
    // to skip travel nobody wants to watch; this is a player asking a mage for something, and a mage
    // that conjures portals from nothing is not a mage.
    SpellInfo const* info = sSpellMgr->GetSpellInfo(match->spellId);
    if (info && !bot->CanNoReagentCast(info))
    {
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (info->Reagent[i] <= 0 || !info->ReagentCount[i])
                continue;

            if (bot->GetItemCount(uint32(info->Reagent[i]), false) < info->ReagentCount[i])
            {
                Refuse(requester, "I'm out of runes -- I can't open that portal.");
                return true;
            }
        }
    }

    if (!botAI->CastSpell(match->spellId, bot))
    {
        // Location is the usual reason, and the core owns that rule rather than this action: rather
        // than hardcode a list of places portals are allowed and get it wrong, let the cast fail and
        // report it.
        Refuse(requester, "I can't cast that here.");
        return true;
    }

    bot->Whisper("Portal to " + match->destination + " opening.", LANG_UNIVERSAL, requester);
    return true;
}
