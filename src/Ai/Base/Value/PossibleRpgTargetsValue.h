/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_POSSIBLERPGTARGETSVALUE_H
#define PLAYERBOTS_POSSIBLERPGTARGETSVALUE_H

#include "NearestGameObjects.h"
#include "NearestUnitsValue.h"
#include "PlayerbotAIConfig.h"
#include "SharedDefines.h"

class PlayerbotAI;

class PossibleRpgTargetsValue : public NearestUnitsValue
{
public:
    PossibleRpgTargetsValue(PlayerbotAI* botAI, float range = 70.0f);

    static std::vector<uint32> allowedNpcFlags;

protected:
    void FindUnits(std::list<Unit*>& targets) override;
    bool AcceptUnit(Unit* unit) override;
};

// Both of the "new rpg" scan values are read every tick by the NewRpg actions, and each read used
// to run a full 150-yard Cell::VisitObjects sweep because CalculatedValue only caches when
// checkInterval >= 2. A 2 second cache is ample: these are questgiver/vendor NPCs and quest
// gameobjects, which do not move, and every consumer re-resolves the GUID and re-validates the
// object before acting on it.
constexpr uint32 NEW_RPG_SCAN_INTERVAL_SECONDS = 2;

class PossibleNewRpgTargetsValue : public NearestUnitsValue
{
public:
    PossibleNewRpgTargetsValue(PlayerbotAI* botAI, float range = 150.0f);

    static std::vector<uint32> allowedNpcFlags;
    GuidVector Calculate() override;
protected:
    void FindUnits(std::list<Unit*>& targets) override;
    bool AcceptUnit(Unit* unit) override;
private:
    float defaultRange;
};

class PossibleNewRpgGameObjectsValue : public ObjectGuidListCalculatedValue
{
public:
    PossibleNewRpgGameObjectsValue(PlayerbotAI* botAI, float range = 150.0f, bool ignoreLos = true)
        : ObjectGuidListCalculatedValue(botAI, "possible new rpg game objects", NEW_RPG_SCAN_INTERVAL_SECONDS),
          range(range),
          ignoreLos(ignoreLos)
    {
        if (allowedGOFlags.empty())
        {
            allowedGOFlags.push_back(GAMEOBJECT_TYPE_QUESTGIVER);
        }
    }

    static std::vector<GameobjectTypes> allowedGOFlags;
    GuidVector Calculate() override;

private:
    float range;
    bool ignoreLos;
};

#endif
