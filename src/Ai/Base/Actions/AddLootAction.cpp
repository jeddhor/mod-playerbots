/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AddLootAction.h"
#include "CellImpl.h"
#include "Event.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LootObjectStack.h"
#include "Playerbots.h"
#include "ServerFacade.h"

bool AddLootAction::Execute(Event event)
{
    ObjectGuid guid = event.getObject();
    if (!guid)
        return false;

    return AI_VALUE(LootObjectStack*, "available loot")->Add(guid);
}

bool AddAllLootAction::Execute(Event /*event*/)
{
    GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
    for (GuidVector::iterator i = gos.begin(); i != gos.end(); i++)
        AddLoot(*i);

    GuidVector corpses = context->GetValue<GuidVector>("nearest corpses")->Get();
    for (GuidVector::iterator i = corpses.begin(); i != corpses.end(); i++)
        AddLoot(*i);

    // Always false, even though the work above did happen.
    //
    // The engine stops at the first action that returns true and records it as what the bot is
    // doing. This action only writes down which corpses exist -- it is instantaneous bookkeeping,
    // not an activity -- but it sits at relevance 5.0, above the 3.0 that NewRpg gives questing,
    // gathering and travel. Returning true therefore ended the tick before any of those could run,
    // so a bot standing anywhere new corpses keep appearing noted them over and over and never got
    // round to doing anything, while the inspector reported "add all loot" as its current action.
    //
    // Reporting false costs nothing: the loot is already in the stack, and the triggers that act on
    // it ("loot available", "can loot") run at 6.0 to 8.0 and still outrank everything else on the
    // next tick. The bot loots exactly as before, and the rest of the tick is free for real work.
    return false;
}

bool AddLootAction::isUseful() { return true; }

bool AddAllLootAction::isUseful() { return true; }

bool AddAllLootAction::AddLoot(ObjectGuid guid) { return AI_VALUE(LootObjectStack*, "available loot")->Add(guid); }

bool AddGatheringLootAction::AddLoot(ObjectGuid guid)
{
    LootObject loot(bot, guid);

    WorldObject* wo = loot.GetWorldObject(bot);
    if (loot.IsEmpty() || !wo)
        return false;

    if (loot.skillId == SKILL_NONE)
        return false;

    if (!loot.IsLootPossible(bot))
        return false;

    return AddAllLootAction::AddLoot(guid);
}
