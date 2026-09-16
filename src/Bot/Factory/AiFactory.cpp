/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AiFactory.h"
#include "LFGMgr.h"
#include "BattlegroundMgr.h"
#include "DKAiObjectContext.h"
#include "DruidAiObjectContext.h"
#include "Engine.h"
#include "Group.h"
#include "HunterAiObjectContext.h"
#include "Item.h"
#include "MageAiObjectContext.h"
#include "PaladinAiObjectContext.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PriestAiObjectContext.h"
#include "RogueAiObjectContext.h"
#include "ShamanAiObjectContext.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WarlockAiObjectContext.h"
#include "WarriorAiObjectContext.h"

namespace
{
/**
 * Did the Dungeon Finder put this character in the group as its tank?
 *
 * Deliberately reads the *group's* record and not LFGMgr's. LFGMgr::GetRoles returns what a player
 * offered, and a bot offers everything its class can do -- a druid or paladin offers tank, healer
 * and damage at once -- so testing it for PLAYER_ROLE_TANK is true for every one of them and would
 * turn the whole class into tanks. The group stores the single role the proposal narrowed to, set
 * by Group::SetLfgRoles when the group formed, which is the actual assignment.
 *
 * Returns false when there is no group or no role recorded, which is the honest answer: outside the
 * Finder nothing has assigned a role and the caller should fall back to reading talents.
 */
bool AssignedTankByLfg(Player* player)
{
    Group* group = player ? player->GetGroup() : nullptr;
    if (!group)
        return false;

    for (Group::MemberSlot const& slot : group->GetMemberSlots())
        if (slot.guid == player->GetGUID())
            return (slot.roles & lfg::PLAYER_ROLE_TANK) != 0;

    return false;
}
}  // namespace


namespace
{
constexpr uint32 SPELL_FROSTFIRE_BOLT = 44614;
constexpr uint32 SPELL_ICE_SHARDS = 15047;
constexpr uint32 SPELL_WHIRLWIND = 1680;
constexpr uint32 SPELL_CAT_FORM = 768;
constexpr uint32 SPELL_DRUID_THICK_HIDE = 16931;
}

AiObjectContext* AiFactory::createAiObjectContext(Player* player, PlayerbotAI* botAI)
{
    switch (player->getClass())
    {
        case CLASS_PRIEST:
            return new PriestAiObjectContext(botAI);
        case CLASS_MAGE:
            return new MageAiObjectContext(botAI);
        case CLASS_WARLOCK:
            return new WarlockAiObjectContext(botAI);
        case CLASS_WARRIOR:
            return new WarriorAiObjectContext(botAI);
        case CLASS_SHAMAN:
            return new ShamanAiObjectContext(botAI);
        case CLASS_PALADIN:
            return new PaladinAiObjectContext(botAI);
        case CLASS_DRUID:
            return new DruidAiObjectContext(botAI);
        case CLASS_HUNTER:
            return new HunterAiObjectContext(botAI);
        case CLASS_ROGUE:
            return new RogueAiObjectContext(botAI);
        case CLASS_DEATH_KNIGHT:
            return new DKAiObjectContext(botAI);
    }

    return new AiObjectContext(botAI);
}

/**
 * The talent tab matching the role this bot has been *told* to play, or -1 if it has not been told.
 *
 * Roles and talents were allowed to disagree, and that is worse than either being wrong. An operator
 * set a priest to heal; the talent roll independently picked Shadow; and because GetPlayerSpecTab
 * reads the tree once any points are spent, every ResetStrategies() -- which a group invite and an
 * LFG join both trigger -- relabelled her DPS and she stopped healing mid-dungeon.
 *
 * Only tank and heal are mapped. Damage is the default for every class, so "assigned DPS" carries no
 * information about which of the two or three damage trees to take, and rolling for it is right.
 */
int8 AiFactory::GetSpecTabForAssignedRole(Player* bot)
{
    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return -1;

    // Deliberately ContainsStrategy rather than IsTank/IsHeal: those consult GetPlayerSpecTab, which
    // consults this, and the pair recurse until the stack runs out. That crash is on record.
    if (botAI->ContainsStrategy(STRATEGY_TYPE_TANK))
    {
        switch (bot->getClass())
        {
            case CLASS_WARRIOR:      return WARRIOR_TAB_PROTECTION;
            case CLASS_PALADIN:      return PALADIN_TAB_PROTECTION;
            case CLASS_DEATH_KNIGHT: return DEATH_KNIGHT_TAB_BLOOD;
            case CLASS_DRUID:        return DRUID_TAB_FERAL;
            default:                 break;
        }
    }

    if (botAI->ContainsStrategy(STRATEGY_TYPE_HEAL))
    {
        switch (bot->getClass())
        {
            case CLASS_PRIEST:  return PRIEST_TAB_HOLY;
            case CLASS_PALADIN: return PALADIN_TAB_HOLY;
            case CLASS_SHAMAN:  return SHAMAN_TAB_RESTORATION;
            case CLASS_DRUID:   return DRUID_TAB_RESTORATION;
            default:            break;
        }
    }

    return -1;
}

uint8 AiFactory::GetPlayerSpecTab(Player* bot)
{
    std::map<uint8, uint32> tabs = GetPlayerSpecTabs(bot);

    if (bot->GetLevel() >= 10 && ((tabs[0] + tabs[1] + tabs[2]) > 0))
    {
        int8 tab = -1;
        uint32 max = 0;
        for (uint32 i = 0; i < uint32(3); i++)
        {
            if (tab == -1 || max < tabs[i])
            {
                tab = i;
                max = tabs[i];
            }
        }
        return tab;
    }
    else
    {
        // No talents spent yet, so the tree cannot say what this bot is. Ask what it has been told
        // to be before falling back to a class default.
        //
        // This is why a low level protection warrior carried a two-hander. Gear scoring penalises
        // 2H heavily for protection specs -- weight x0.5 then x0.1 -- but keys that on the talent
        // tab, and with no talents a warrior fell through to tab 0, which is Arms, which wants a
        // two-hander. Setting the tank strategies made no difference because nothing consulted
        // them.
        //
        // Ask the strategies directly, NOT through PlayerbotAI::IsTank. IsTank only short-circuits
        // to ContainsStrategy when the player has a bot AI; without one it falls through to
        // GetPlayerSpecTab -- this function -- and the pair recurse until the stack runs out. That
        // is not hypothetical: it segfaulted the realm five times, every crash landing one word
        // from the stack pointer.
        PlayerbotAI* const botAi = GET_PLAYERBOT_AI(bot);

        if (botAi && botAi->ContainsStrategy(STRATEGY_TYPE_TANK))
        {
            switch (bot->getClass())
            {
                case CLASS_WARRIOR:
                    return WARRIOR_TAB_PROTECTION;
                case CLASS_PALADIN:
                    return PALADIN_TAB_PROTECTION;
                case CLASS_DEATH_KNIGHT:
                    return DEATH_KNIGHT_TAB_BLOOD;
                case CLASS_DRUID:
                    return DRUID_TAB_FERAL;
                default:
                    break;
            }
        }

        uint8 tab = 0;

        switch (bot->getClass())
        {
            case CLASS_MAGE:
                tab = MAGE_TAB_FROST;
                break;
            case CLASS_PALADIN:
                tab = PALADIN_TAB_RETRIBUTION;
                break;
            case CLASS_PRIEST:
                tab = PRIEST_TAB_HOLY;
                break;
            case CLASS_WARLOCK:
                tab = WARLOCK_TAB_DEMONOLOGY;
                break;
        }

        return tab;
    }
}

std::map<uint8, uint32> AiFactory::GetPlayerSpecTabs(Player* bot)
{
    std::map<uint8, uint32> tabs = {{0, 0}, {0, 0}, {0, 0}};
    const PlayerTalentMap& talentMap = bot->GetTalentMap();
    for (PlayerTalentMap::const_iterator i = talentMap.begin(); i != talentMap.end(); ++i)
    {
        uint32 spellId = i->first;
        if ((bot->GetActiveSpecMask() & i->second->specMask) == 0)
        {
            continue;
        }
        TalentSpellPos const* talentPos = GetTalentSpellPos(spellId);
        if (!talentPos)
            continue;
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentPos->talent_id);
        if (!talentInfo)
            continue;

        uint32 const* talentTabIds = GetTalentTabPages(bot->getClass());

        const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        int rank = spellInfo ? spellInfo->GetRank() : 1;
        if (talentInfo->TalentTab == talentTabIds[0])
            tabs[0] += rank;
        if (talentInfo->TalentTab == talentTabIds[1])
            tabs[1] += rank;
        if (talentInfo->TalentTab == talentTabIds[2])
            tabs[2] += rank;
    }
    return tabs;
}

BotRoles AiFactory::GetPlayerRoles(Player* player)
{
    BotRoles role = BOT_ROLE_NONE;
    uint8 tab = GetPlayerSpecTab(player);

    switch (player->getClass())
    {
        case CLASS_PRIEST:
            if (tab == PRIEST_TAB_SHADOW)
                role = BOT_ROLE_DPS;
            else
                role = BOT_ROLE_HEALER;
            break;
        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_RESTORATION)
                role = BOT_ROLE_HEALER;
            else
                role = BOT_ROLE_DPS;
            break;
        case CLASS_WARRIOR:
            if (tab == WARRIOR_TAB_PROTECTION)
                role = BOT_ROLE_TANK;
            else
                role = BOT_ROLE_DPS;
            break;
        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_HOLY)
                role = BOT_ROLE_HEALER;
            else if (tab == PALADIN_TAB_PROTECTION)
                role = BOT_ROLE_TANK;
            else if (tab == PALADIN_TAB_RETRIBUTION)
                role = BOT_ROLE_DPS;
            break;
        case CLASS_DRUID:
            if (tab == DRUID_TAB_BALANCE)
                role = BOT_ROLE_DPS;
            else if (tab == DRUID_TAB_FERAL)
                role = (BotRoles)(BOT_ROLE_TANK | BOT_ROLE_DPS);
            else if (tab == DRUID_TAB_RESTORATION)
                role = BOT_ROLE_HEALER;
            break;
        default:
            role = BOT_ROLE_DPS;
            break;
    }

    return role;
}

std::string AiFactory::GetPlayerSpecName(Player* player)
{
    std::string specName;
    int tab = GetPlayerSpecTab(player);
    switch (player->getClass())
    {
        case CLASS_PRIEST:
            if (tab == PRIEST_TAB_SHADOW)
                specName = "shadow";
            else if (tab == PRIEST_TAB_HOLY)
                specName = "holy";
            else
                specName = "disc";
            break;
        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_RESTORATION)
                specName = "resto";
            else if (tab == SHAMAN_TAB_ENHANCEMENT)
                specName = "enhance";
            else
                specName = "elem";
            break;
        case CLASS_WARRIOR:
            if (tab == WARRIOR_TAB_PROTECTION)
                specName = "prot";
            else if (tab == WARRIOR_TAB_FURY)
                specName = "fury";
            else
                specName = "arms";
            break;
        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_HOLY)
                specName = "holy";
            else if (tab == PALADIN_TAB_PROTECTION)
                specName = "prot";
            else if (tab == PALADIN_TAB_RETRIBUTION)
                specName = "retrib";
            break;
        case CLASS_DRUID:
            if (tab == DRUID_TAB_BALANCE)
                specName = "balance";
            else if (tab == DRUID_TAB_FERAL)
                specName = "feraldps";
            else if (tab == DRUID_TAB_RESTORATION)
                specName = "resto";
            break;
        case CLASS_ROGUE:
            if (tab == ROGUE_TAB_ASSASSINATION)
                specName = "assas";
            else if (tab == ROGUE_TAB_COMBAT)
                specName = "combat";
            else if (tab == ROGUE_TAB_SUBTLETY)
                specName = "subtle";
            break;
        case CLASS_HUNTER:
            if (tab == HUNTER_TAB_BEAST_MASTERY)
                specName = "beast";
            else if (tab == HUNTER_TAB_MARKSMANSHIP)
                specName = "marks";
            else if (tab == HUNTER_TAB_SURVIVAL)
                specName = "surv";
            break;
        case CLASS_DEATH_KNIGHT:
            if (tab == DEATH_KNIGHT_TAB_BLOOD)
                specName = "blooddps";
            else if (tab == DEATH_KNIGHT_TAB_FROST)
                specName = "frostdps";
            else if (tab == DEATH_KNIGHT_TAB_UNHOLY)
                specName = "unholydps";
            break;
        case CLASS_MAGE:
            if (tab == MAGE_TAB_ARCANE)
                specName = "arcane";
            else if (tab == MAGE_TAB_FIRE)
                specName = "fire";
            else if (tab == MAGE_TAB_FROST)
                specName = "frost";
            break;
        case CLASS_WARLOCK:
            if (tab == WARLOCK_TAB_AFFLICTION)
                specName = "afflic";
            else if (tab == WARLOCK_TAB_DEMONOLOGY)
                specName = "demo";
            else if (tab == WARLOCK_TAB_DESTRUCTION)
                specName = "destro";
            break;
        default:
            break;
    }

    return specName;
}

void AiFactory::AddDefaultCombatStrategies(Player* player, PlayerbotAI* const facade, Engine* engine)
{
    uint8 tab = GetPlayerSpecTab(player);

    if (!player->InBattleground())
        engine->addStrategiesNoInit("racials", "chat", "default", "cast time", "potions", "duel", "boost", nullptr);

    if (sPlayerbotAIConfig.autoAvoidAoe && facade->HasGameClientMaster())
        engine->addStrategy("avoid aoe", false);

    engine->addStrategy("formation", false);

    switch (player->getClass())
    {
        case CLASS_PRIEST:
            if (tab == PRIEST_TAB_SHADOW)
                engine->addStrategiesNoInit("dps", "shadow debuff", "shadow aoe", nullptr);
            else if (tab == PRIEST_TAB_DISCIPLINE)
                engine->addStrategy("heal", false);
            else // if (tab == PRIEST_TAB_HOLY)
                engine->addStrategy("holy heal", false);

            engine->addStrategiesNoInit("dps assist", "cure", nullptr);
            break;
        case CLASS_MAGE:
            if (tab == MAGE_TAB_ARCANE)
                engine->addStrategiesNoInit("arcane", "bdps", nullptr);
            else if (tab == MAGE_TAB_FIRE)
            {
                if (player->HasSpell(SPELL_FROSTFIRE_BOLT) && player->HasAura(SPELL_ICE_SHARDS))
                    engine->addStrategiesNoInit("frostfire", "bdps", nullptr);
                else
                    engine->addStrategiesNoInit("fire", "bdps", nullptr);
            }
            else // if (tab == MAGE_TAB_FROST)
                engine->addStrategiesNoInit("frost", "bmana", nullptr);

            engine->addStrategiesNoInit("dps", "dps assist", "cure", "cc", "aoe", nullptr);
            break;
        case CLASS_WARRIOR:
            if (tab == WARRIOR_TAB_PROTECTION || AssignedTankByLfg(player))
                engine->addStrategiesNoInit("tank", "tank assist", "pull", "pull back", "aoe", nullptr);
            else if (tab == WARRIOR_TAB_ARMS || !player->HasSpell(SPELL_WHIRLWIND))
                engine->addStrategiesNoInit("arms", "aoe", "dps assist", nullptr);
            else // if (tab == WARRIOR_TAB_FURY)
                engine->addStrategiesNoInit("fury", "aoe", "dps assist", nullptr);
            break;
        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_ELEMENTAL)
                engine->addStrategiesNoInit("ele", "stoneskin", "wrath", "mana spring", "wrath of air", nullptr);
            else if (tab == SHAMAN_TAB_RESTORATION)
                engine->addStrategiesNoInit("resto", "stoneskin", "flametongue", "mana spring", "wrath of air", nullptr);
            else // if (tab == SHAMAN_TAB_ENHANCEMENT)
                engine->addStrategiesNoInit("enh", "strength of earth", "magma", "healing stream", "windfury", nullptr);

            engine->addStrategiesNoInit("dps assist", "cure", "aoe", nullptr);
            break;
        case CLASS_PALADIN:
            // Spec alone decided this, so a paladin the Finder placed as the tank fought as
            // retribution -- no Righteous Fury, no threat, in a dungeon that needed a tank.
            if (tab == PALADIN_TAB_PROTECTION || AssignedTankByLfg(player))
                engine->addStrategiesNoInit("tank", "tank assist", "pull", "pull back", "bthreat", "barmor", "cure", nullptr);
            else if (tab == PALADIN_TAB_HOLY)
                engine->addStrategiesNoInit("heal", "dps assist", "cure", "bcast", nullptr);
            else // if (tab == PALADIN_TAB_RETRIBUTION)
                engine->addStrategiesNoInit("dps", "dps assist", "cure", "baoe", nullptr);
            break;
        case CLASS_DRUID:
            if (tab == DRUID_TAB_BALANCE)
            {
                engine->addStrategiesNoInit("balance", "cure", "aoe", "cc", "dps assist", nullptr);
            }
            else if (tab == DRUID_TAB_RESTORATION)
                engine->addStrategiesNoInit("resto", "cure", "dps assist", "tranquility", nullptr);
            else
            {
                // Whether a feral druid tanks was decided entirely by talents: cat unless it had
                // Thick Hide, which sits deep in the tree. So every feral druid below that talent
                // was given the dps strategy no matter what its group needed, and a level 25 druid
                // sent into Wailing Caverns as the tank fought in cat form.
                //
                // PlayerbotAI::IsTank cannot answer this. For a druid it reports true only when the
                // character is *already* in bear form -- and the strategy that shifts to bear is the
                // one being chosen here, so nothing ever breaks the circle.
                //
                // The Dungeon Finder's own assignment does answer it, and it is the authority:
                // LfgRolesFor advertises a druid as able to tank, the queue places it on that basis,
                // and this is the one place that was not told. Outside the Finder there is no such
                // assignment and the talent heuristic is still the best guess available.
                bool const tankRole = AssignedTankByLfg(player) ||
                                      player->HasAura(SPELL_DRUID_THICK_HIDE) ||
                                      !player->HasSpell(SPELL_CAT_FORM);

                if (tankRole)
                    engine->addStrategiesNoInit("bear", "tank assist", "pull", "pull back", "feral charge", nullptr);
                else
                    engine->addStrategiesNoInit("cat", "aoe", "cc", "dps assist", "feral charge", nullptr);
            }
            break;
        case CLASS_HUNTER:
            if (tab == HUNTER_TAB_BEAST_MASTERY)
                engine->addStrategy("bm", false);
            else if (tab == HUNTER_TAB_MARKSMANSHIP)
                engine->addStrategy("mm", false);
            else // if (tab == HUNTER_TAB_SURVIVAL)
                engine->addStrategy("surv", false);

            engine->addStrategiesNoInit("cc", "dps assist", "aoe", "bdps", nullptr);
            break;
        case CLASS_ROGUE:
            if (tab == ROGUE_TAB_ASSASSINATION || tab == ROGUE_TAB_SUBTLETY)
                engine->addStrategiesNoInit("melee", "dps assist", "aoe", nullptr);
            else // if (tab == ROGUE_TAB_COMBAT)
                engine->addStrategiesNoInit("dps", "dps assist", "aoe", nullptr);
            break;
        case CLASS_WARLOCK:
            if (tab == WARLOCK_TAB_AFFLICTION)
                engine->addStrategiesNoInit("affli", "curse of agony", nullptr);
            else if (tab == WARLOCK_TAB_DEMONOLOGY)
                engine->addStrategiesNoInit("demo", "curse of agony", "meta melee", nullptr);
            else // if (tab == WARLOCK_TAB_DESTRUCTION)
                engine->addStrategiesNoInit("destro", "curse of elements", nullptr);

            engine->addStrategiesNoInit("cc", "dps assist", "aoe", nullptr);
            break;
        case CLASS_DEATH_KNIGHT:
            if (tab == DEATH_KNIGHT_TAB_BLOOD)
                engine->addStrategiesNoInit("blood", "tank assist", "pull", "pull back", nullptr);
            else if (tab == DEATH_KNIGHT_TAB_FROST)
                engine->addStrategiesNoInit("frost", "frost aoe", "dps assist", nullptr);
            else // if (tab == DEATH_KNIGHT_TAB_UNHOLY)
                engine->addStrategiesNoInit("unholy", "unholy aoe", "dps assist", nullptr);
            break;
    }

    if (PlayerbotAI::IsTank(player, true))
        engine->addStrategy("tank face", false);

    if (PlayerbotAI::IsMelee(player, true) && PlayerbotAI::IsDps(player, true))
        engine->addStrategy("behind", false);

    if (PlayerbotAI::IsHeal(player, true))
    {
        if (sPlayerbotAIConfig.autoSaveMana)
            engine->addStrategy("save mana", false);
        if (!sPlayerbotAIConfig.IsRestrictedHealerDPSMap(player->GetMapId()))
            engine->addStrategy("healer dps", false);
    }

    if (IsSelfBot(player) || sRandomPlayerbotMgr.IsRandomBot(player))
    {
        if (!player->GetGroup())
        {
            // change for heal spec
            engine->addStrategy("boost", false);
            engine->addStrategy("dps assist", false);
            engine->removeStrategy("threat", false);

            switch (player->getClass())
            {
                case CLASS_PRIEST:
                {
                    if (tab != PRIEST_TAB_SHADOW)
                        engine->addStrategiesNoInit("holy dps", "shadow debuff", "shadow aoe", nullptr);
                    break;
                }
                case CLASS_DRUID:
                {
                    if (tab == DRUID_TAB_RESTORATION)
                    {
                        engine->addStrategiesNoInit("aoe", nullptr);
                    }
                    break;
                }
                case CLASS_SHAMAN:
                {
                    if (tab == SHAMAN_TAB_RESTORATION)
                        engine->addStrategiesNoInit("caster", "caster aoe", nullptr);
                    break;
                }
                case CLASS_PALADIN:
                {
                    if (tab == PALADIN_TAB_HOLY)
                        engine->addStrategiesNoInit("dps", "dps assist", "baoe", nullptr);
                    break;
                }
                default:
                    break;
            }
        }
    }
    if (sRandomPlayerbotMgr.IsRandomBot(player))
        engine->ChangeStrategy(sPlayerbotAIConfig.randomBotCombatStrategies);
    else
        engine->ChangeStrategy(sPlayerbotAIConfig.combatStrategies);

    // Battleground switch
    if (player->InBattleground() && player->GetBattleground())
    {
        BattlegroundTypeId bgType = player->GetBattlegroundTypeId();
        if (bgType == BATTLEGROUND_RB)
            bgType = player->GetBattleground()->GetBgTypeID(true);

        if (bgType == BATTLEGROUND_WS)
            engine->addStrategy("warsong", false);

        if (bgType == BATTLEGROUND_AB)
            engine->addStrategy("arathi", false);

        if (bgType == BATTLEGROUND_AV)
            engine->addStrategy("alterac", false);

        if (bgType == BATTLEGROUND_EY)
            engine->addStrategy("eye", false);

        if (bgType == BATTLEGROUND_IC)
            engine->addStrategy("isle", false);

        if (player->InArena())
        {
            engine->addStrategy("arena", false);
            engine->addStrategiesNoInit("boost", "racials", "chat", "default", "aoe", "cast time", "dps assist", nullptr);
        }
        else
            engine->addStrategiesNoInit("boost", "racials", "chat", "default", "aoe", "potions", "cast time", "dps assist", nullptr);

        engine->removeStrategy("custom::say", false);
        engine->removeStrategy("flee", false);
        engine->removeStrategy("threat", false);
        engine->addStrategy("boost", false);
    }
}

Engine* AiFactory::createCombatEngine(Player* player, PlayerbotAI* const facade, AiObjectContext* aiObjectContext)
{
    Engine* engine = new Engine(facade, aiObjectContext);
    AddDefaultCombatStrategies(player, facade, engine);
    engine->Init();
    return engine;
}

void AiFactory::AddDefaultNonCombatStrategies(Player* player, PlayerbotAI* const facade, Engine* nonCombatEngine)
{
    uint8 tab = GetPlayerSpecTab(player);

    switch (player->getClass())
    {
        case CLASS_PRIEST:
            nonCombatEngine->addStrategiesNoInit("dps assist", "cure", "rshadow", nullptr);
            break;
        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_PROTECTION)
            {
                nonCombatEngine->addStrategiesNoInit("bthreat", "tank assist", "pull", "barmor", nullptr);
                if (player->GetLevel() >= 20)
                    nonCombatEngine->addStrategy("bsanc", false);
                else
                    nonCombatEngine->addStrategy("bmight", false);
            }
            else if (tab == PALADIN_TAB_HOLY)
                nonCombatEngine->addStrategiesNoInit("dps assist", "bwisdom", "bcast", nullptr);
            else
                nonCombatEngine->addStrategiesNoInit("dps assist", "bmight", "baoe", nullptr);

            nonCombatEngine->addStrategiesNoInit("cure", nullptr);
            break;
        case CLASS_HUNTER:
            nonCombatEngine->addStrategiesNoInit("bdps", "dps assist", "pet", nullptr);
            break;
        case CLASS_SHAMAN:
            nonCombatEngine->addStrategiesNoInit("dps assist", "cure", nullptr);
            break;
        case CLASS_MAGE:
            if (tab == MAGE_TAB_ARCANE || tab == MAGE_TAB_FIRE)
                nonCombatEngine->addStrategy("bdps", false);
            else
                nonCombatEngine->addStrategy("bmana", false);

            nonCombatEngine->addStrategiesNoInit("dps assist", "cure", nullptr);
            break;
        case CLASS_DRUID:
            if (tab == DRUID_TAB_FERAL)
            {
                if (player->GetLevel() >= 20 && !player->HasAura(SPELL_DRUID_THICK_HIDE))
                    nonCombatEngine->addStrategy("dps assist", false);
                else
                    nonCombatEngine->addStrategiesNoInit("tank assist", "pull", nullptr);
            }
            else
                nonCombatEngine->addStrategiesNoInit("dps assist", "cure", nullptr);
            break;
        case CLASS_WARRIOR:
            if (tab == WARRIOR_TAB_PROTECTION)
                nonCombatEngine->addStrategiesNoInit("tank assist", "pull", nullptr);
            else
                nonCombatEngine->addStrategy("dps assist", false);
            break;
        case CLASS_WARLOCK:
            if (tab == WARLOCK_TAB_AFFLICTION)
                nonCombatEngine->addStrategiesNoInit("felhunter", "spellstone", nullptr);
            else if (tab == WARLOCK_TAB_DEMONOLOGY)
                nonCombatEngine->addStrategiesNoInit("felguard", "spellstone", nullptr);
            else if (tab == WARLOCK_TAB_DESTRUCTION)
                nonCombatEngine->addStrategiesNoInit("imp", "firestone", nullptr);

            nonCombatEngine->addStrategiesNoInit("dps assist", "ss self", nullptr);
            break;
        case CLASS_DEATH_KNIGHT:
            if (tab == DEATH_KNIGHT_TAB_BLOOD)
                nonCombatEngine->addStrategiesNoInit("tank assist", "pull", nullptr);
            else
                nonCombatEngine->addStrategy("dps assist", false);
            break;
        default:
            nonCombatEngine->addStrategy("dps assist", false);
            break;
    }

    if (!player->InBattleground())
    {
        nonCombatEngine->addStrategiesNoInit("nc", "food", "chat", "follow", "default", "force rebuff", "quest", "loot",
                                            "gather", "duel", "pvp", "buff", "mount", "emote", nullptr);
    }

    if (sPlayerbotAIConfig.autoSaveMana && PlayerbotAI::IsHeal(player, true))
        nonCombatEngine->addStrategy("save mana", false);

    // Out of combat too. Only the combat engine carried this, so a bot resting or following with no
    // enemy about stood in a campfire and burned for as long as it was left there -- Alee did, until
    // a pull put her back in the combat engine and she finally stepped out.
    if (sPlayerbotAIConfig.autoAvoidAoe && facade->HasGameClientMaster())
        nonCombatEngine->addStrategy("avoid aoe", false);

    // Autonomy is decided by SITUATION, not by how the bot came to exist.
    //
    // This used to read `IsRandomBot(player)`, so only random bots were given the `grind` and
    // `new rpg` strategies. An alt bot standing outside a party, or a player turned into a self
    // bot, fell through to a much thinner strategy set and visibly did less than a random bot next
    // to it - which is also why self-bot behaviour reports were hard to reason about: they were
    // observations of a different strategy set, not of the same code behaving differently.
    //
    // An unsupervised bot is one with no human master. That covers random bots, unparried alt bots
    // and self bots alike (R8.1, R8.2), and it is the condition the autonomous strategies were
    // always really about.
    Player* aiMaster = facade ? facade->GetMaster() : nullptr;
    bool const humanSupervised = aiMaster && !GET_PLAYERBOT_AI(aiMaster);
    bool const unsupervised = sPlayerbotAIConfig.botTypeParity
                                  ? !humanSupervised
                                  : sRandomPlayerbotMgr.IsRandomBot(player);

    if (unsupervised && !player->InBattleground())
    {
        Player* master = facade->GetMaster();

        // let 25% of free bots start duels.
        if (!urand(0, 3))
            nonCombatEngine->addStrategy("start duel", false);

        if (sPlayerbotAIConfig.randomBotJoinLfg)
            nonCombatEngine->addStrategy("lfg", false);

        if (!player->GetGroup() || player->GetGroup()->GetLeaderGUID() == player->GetGUID())
        {
            // let 25% of random not grouped (or grp leader) bots help other players
            // if (!urand(0, 3))
            //     nonCombatEngine->addStrategy("attack tagged");

            // nonCombatEngine->addStrategy("pvp", false);
            // nonCombatEngine->addStrategy("collision");
            // nonCombatEngine->addStrategy("guild");

            // Bots have always been able to invite each other -- InviteNearbyToGroupAction, the
            // GrouperType per bot, and a +/-2 level guard on invitations all exist -- but the
            // strategy that drives it was commented out here, so no bot ever grouped on its own.
            //
            // A solo bot dying repeatedly to an elite is the most visible way the world looks
            // broken, and P1.1's blacklist makes it worse: the quest gets written off as unworkable
            // when the real problem was that nobody came to help.
            if (sPlayerbotAIConfig.randomBotGroupNearby)
                nonCombatEngine->addStrategy("group", false);

            nonCombatEngine->addStrategy("grind", false);

            if (sPlayerbotAIConfig.enableNewRpgStrategy)
                nonCombatEngine->addStrategy("new rpg", false);
            else if (sPlayerbotAIConfig.autoDoQuests)
            {
                // nonCombatEngine->addStrategy("travel");
                nonCombatEngine->addStrategy("rpg", false);
            }
            else
                nonCombatEngine->addStrategy("move random", false);

            if (sPlayerbotAIConfig.randomBotJoinBG)
                nonCombatEngine->addStrategy("bg", false);

            // if (!master || GET_PLAYERBOT_AI(master))
            //     nonCombatEngine->addStrategy("maintenance");

            nonCombatEngine->ChangeStrategy(sPlayerbotAIConfig.randomBotNonCombatStrategies);
        }
        else
        {
            if (facade)
            {
                if (master)
                {
                    PlayerbotAI* masterBotAI = GET_PLAYERBOT_AI(master);
                    if (masterBotAI || sRandomPlayerbotMgr.IsRandomBot(player))
                    {
                        // nonCombatEngine->addStrategy("pvp", false);
                        // nonCombatEngine->addStrategy("collision");
                        // nonCombatEngine->addStrategy("group");
                        // nonCombatEngine->addStrategy("guild");

                        // if (sPlayerbotAIConfig.autoDoQuests)
                        // {
                        //     // nonCombatEngine->addStrategy("travel");
                        //     nonCombatEngine->addStrategy("rpg");
                        // }
                        // else
                        // {
                        //     nonCombatEngine->addStrategy("move random");
                        // }

                        // if (masterBotAI)
                        //     nonCombatEngine->addStrategy("maintenance");

                        nonCombatEngine->ChangeStrategy(sPlayerbotAIConfig.randomBotNonCombatStrategies);
                    }
                    else
                    {
                        // nonCombatEngine->addStrategy("pvp", false);
                        nonCombatEngine->ChangeStrategy(sPlayerbotAIConfig.nonCombatStrategies);
                    }
                }
            }
        }
    }
    else
        nonCombatEngine->ChangeStrategy(sPlayerbotAIConfig.nonCombatStrategies);

    // Battleground switch
    if (player->InBattleground() && player->GetBattleground())
    {
        nonCombatEngine->addStrategiesNoInit("nc", "chat", "default", "force rebuff", "buff", "food", "mount", "pvp",
                                       "dps assist", "attack tagged", "emote", nullptr);
        nonCombatEngine->removeStrategy("custom::say", false);
        nonCombatEngine->removeStrategy("travel", false);
        nonCombatEngine->removeStrategy("rpg", false);
        nonCombatEngine->removeStrategy("grind", false);

        BattlegroundTypeId bgType = player->GetBattlegroundTypeId();
        if (bgType == BATTLEGROUND_RB)
            bgType = player->GetBattleground()->GetBgTypeID(true);

        if ((bgType <= BATTLEGROUND_EY || bgType == BATTLEGROUND_IC) &&
            !player->InArena())  // do not add for not supported bg or arena
            nonCombatEngine->addStrategy("battleground", false);

        if (bgType == BATTLEGROUND_WS)
            nonCombatEngine->addStrategy("warsong", false);

        if (bgType == BATTLEGROUND_AV)
            nonCombatEngine->addStrategy("alterac", false);

        if (bgType == BATTLEGROUND_AB)
            nonCombatEngine->addStrategy("arathi", false);

        if (bgType == BATTLEGROUND_EY)
            nonCombatEngine->addStrategy("eye", false);

        if (bgType == BATTLEGROUND_IC)
            nonCombatEngine->addStrategy("isle", false);

        if (player->InArena())
        {
            nonCombatEngine->addStrategy("arena", false);
            nonCombatEngine->removeStrategy("mount", false);
        }
    }
}

Engine* AiFactory::createNonCombatEngine(Player* player, PlayerbotAI* const facade, AiObjectContext* aiObjectContext)
{
    Engine* nonCombatEngine = new Engine(facade, aiObjectContext);

    AddDefaultNonCombatStrategies(player, facade, nonCombatEngine);
    nonCombatEngine->Init();
    return nonCombatEngine;
}

void AiFactory::AddDefaultDeadStrategies(Player* player, PlayerbotAI* const facade, Engine* deadEngine)
{
    (void)facade;  // unused and remove warning
    deadEngine->addStrategiesNoInit("dead", "stay", "chat", "default", "follow", nullptr);

    if (sRandomPlayerbotMgr.IsRandomBot(player) && !player->GetGroup())
        deadEngine->removeStrategy("follow", false);
}

Engine* AiFactory::createDeadEngine(Player* player, PlayerbotAI* const facade, AiObjectContext* AiObjectContext)
{
    Engine* deadEngine = new Engine(facade, AiObjectContext);
    AddDefaultDeadStrategies(player, facade, deadEngine);
    deadEngine->Init();
    return deadEngine;
}
