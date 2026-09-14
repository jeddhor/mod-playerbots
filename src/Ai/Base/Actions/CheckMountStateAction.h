/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHECKMOUNTSTATEACTION_H
#define PLAYERBOTS_CHECKMOUNTSTATEACTION_H

#include "UseItemAction.h"
#include <unordered_map>
#include <vector>

const uint16 SPELL_TRAVEL_FORM = 783;
const uint16 SPELL_FLIGHT_FORM = 33943;
const uint16 SPELL_SWIFT_FLIGHT_FORM = 40120;

struct MountData
{
    bool swiftMount = false;
    // Outer map: index (0 for ground, 1 for flight), inner map: effect speed -> vector of spell IDs.
    std::map<uint32, std::map<int32, std::vector<uint32>>> allSpells;
    // Default mount speed.
    int32 maxSpeed = 59;
};

struct PreferredMountCache
{
    std::vector<uint32> groundMounts;
    std::vector<uint32> flightMounts;
};

class PlayerbotAI;

class CheckMountStateAction : public UseItemAction
{
public:
    CheckMountStateAction(PlayerbotAI* botAI) : UseItemAction(botAI, "check mount state", true) {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override { return true; }
    bool Mount();

    static void CompleteDismount(Player* bot);

private:
    Player* master;
    ShapeshiftForm masterInShapeshiftForm;
    ShapeshiftForm botInShapeshiftForm;
    static std::unordered_map<uint32, PreferredMountCache> mountCache;
    static bool preferredMountTableChecked;
    float CalculateDismountDistance() const;
    float CalculateMountDistance() const;
    void Dismount();
    void ClearStaleFlightFlags();
    bool ShouldFollowMasterMountState(Player* master, bool noAttackers, bool shouldMount) const;
    bool ShouldDismountForMaster(Player* master) const;
    int32 CalculateMasterMountSpeed(Player* master) const;
    bool CheckForSwiftMount() const;
    std::map<uint32, std::map<int32, std::vector<uint32>>> GetAllMountSpells() const;
    bool TryForms(Player* master, int32 masterMountType, int32 masterSpeed) const;
    bool TryPreferredMount(Player* master) const;
    uint32 GetMountType(Player* master) const;
    bool TryRandomMountFiltered(const std::map<int32, std::vector<uint32>>& spells, int32 masterSpeed) const;

    /**
     * P12.6 -- how much of a boast a mount is, 1 (best) to 3, by how it was obtained.
     *
     * Quality is useless for this: 235 of 297 mounts are epic, so it cannot tell Ashes of Al'ar
     * from a Blue Mechanostrider. What separates them is where they come from, which is what the
     * requirement actually says -- drops and achievements ahead of reputation, reputation ahead of
     * anything a vendor will simply sell you.
     *
     * Built once from item_template and npc_vendor: 120 drop/achievement, 40 reputation, 137 vendor.
     */
    static uint8 MountPrestige(uint32 mountSpellId);
    static void EnsureMountPrestigeLoaded();

    static std::unordered_map<uint32, uint8> mountPrestige;
    static bool mountPrestigeLoaded;
};

#endif
