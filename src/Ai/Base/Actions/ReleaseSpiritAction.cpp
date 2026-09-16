/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ReleaseSpiritAction.h"

#include <unordered_set>

#include "Corpse.h"
#include "Event.h"
#include "GameGraveyard.h"
#include "Log.h"
#include "NearestNpcsValue.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "PlayerbotAIConfig.h"
#include "Group.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "Spell.h"

// ReleaseSpiritAction implementation
bool ReleaseSpiritAction::Execute(Event event)
{
    if (bot->IsAlive())
    {
        if (!bot->InBattleground())
        {
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "release_spirit_not_dead_wait", "I am not dead, will wait here", {}));
            // -follow in bg is overwriten each tick with +follow
            // +stay in bg causes stuttering effect as bot is cycled between +stay and +follow each tick
            botAI->ChangeStrategy("-follow,+stay", BOT_STATE_NON_COMBAT);
        }

        return false;
    }

    if (bot->GetCorpse() && bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "release_spirit_already_spirit", "I am already a spirit", {}));
        return false;
    }

    const WorldPacket& packet = event.getPacket();
    const std::string message = !packet.empty() && packet.GetOpcode() == CMSG_REPOP_REQUEST
        ? PlayerbotTextMgr::instance().GetBotTextOrDefault("release_spirit_releasing", "Releasing...", {})
        : PlayerbotTextMgr::instance().GetBotTextOrDefault("release_spirit_meet_graveyard", "Meet me at the graveyard", {});
    botAI->TellMasterNoFacing(message);

    IncrementDeathCount();
    // Free on resurrection for random and alt bots, which is what keeps them serviceable without
    // errands. A self bot is excluded: it pays for its own repairs through BotRepairMgr, and a free
    // one here would quietly refund every death.
    if (!IsSelfBot(bot))
        bot->DurabilityRepairAll(false, 1.0f, false);
    LogRelease("released");

    WorldPacket releasePacket(CMSG_REPOP_REQUEST);
    releasePacket << uint8(0);
    bot->GetSession()->HandleRepopRequestOpcode(releasePacket);

    return true;
}

void ReleaseSpiritAction::IncrementDeathCount() const
{
    // Death Count to prevent skeleton piles
    Player* master = botAI->GetMaster();
    if (!master || GET_PLAYERBOT_AI(master))
    {
        uint32 deathCount = AI_VALUE(uint32, "death count");
        context->GetValue<uint32>("death count")->Set(deathCount + 1);
    }
}

void ReleaseSpiritAction::LogRelease(const std::string& releaseMsg) const
{
    const std::string teamPrefix = bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H";

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> {}",
        bot->GetGUID().ToString().c_str(),
        teamPrefix,
        bot->GetLevel(),
        bot->GetName().c_str(),
        releaseMsg.c_str());
}

// AutoReleaseSpiritAction implementation
bool AutoReleaseSpiritAction::Execute(Event /*event*/)
{
    IncrementDeathCount();
    // Free on resurrection for random and alt bots, which is what keeps them serviceable without
    // errands. A self bot is excluded: it pays for its own repairs through BotRepairMgr, and a free
    // one here would quietly refund every death.
    if (!IsSelfBot(bot))
        bot->DurabilityRepairAll(false, 1.0f, false);
    LogRelease("auto released");

    WorldPacket packet(CMSG_REPOP_REQUEST);
    packet << uint8(0);
    bot->GetSession()->HandleRepopRequestOpcode(packet);

    LogRelease("releases spirit");

    if (bot->InBattleground())
    {
        return HandleBattlegroundSpiritHealer();
    }

    botAI->SetNextCheckDelay(1000);
    return true;
}

bool AutoReleaseSpiritAction::isUseful()
{
    // A person steering their own ghost outranks the corpse run. This action teleports and walks
    // without going through the movement layer, so the CanMove() gate never saw it -- which is how a
    // character kept setting off for its corpse while its owner pressed stop over and over.
    if (botAI->HumanIsDriving())
        return false;

    if (!bot->isDead())
    {
        _deathSeenMs = 0;
        return false;
    }

    if (bot->InArena())
        return false;

    if (bot->InBattleground())
        return ShouldDelayBattlegroundRelease();

    if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        return false;

    if (WaitingForResurrection())
        return false;

    return ShouldAutoRelease();
}

namespace
{
/// Every spell that brings a dead player back, computed once from the spell store.
std::unordered_set<uint32> const& ResurrectionSpells()
{
    static std::unordered_set<uint32> const spells = []()
    {
        std::unordered_set<uint32> found;
        for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id))
                if (info->HasEffect(SPELL_EFFECT_RESURRECT))
                    found.insert(id);
        return found;
    }();
    return spells;
}

bool KnowsResurrection(Player* player)
{
    std::unordered_set<uint32> const& spells = ResurrectionSpells();
    for (auto const& [spellId, playerSpell] : player->GetSpellMap())
        if (playerSpell && playerSpell->State != PLAYERSPELL_REMOVED && playerSpell->Active && spells.count(spellId))
            return true;
    return false;
}
}  // namespace

/**
 * A groupmate can bring this bot back, so do not run to the graveyard yet.
 *
 * Released at once, a bot threw away a resurrection that was already on its way: Lucillai died,
 * Alee began casting Resurrection two seconds later, and four seconds after the death -- mid-cast --
 * Lucillai released and set off on a corpse run. Waits while a resurrection is being cast at the bot
 * or has been offered, and for a short grace period while an out-of-combat groupmate nearby knows
 * one and may be about to start.
 */
bool AutoReleaseSpiritAction::WaitingForResurrection()
{
    // Already offered. Accepting it is the dead strategy's job; releasing now would refuse it.
    if (bot->isResurrectRequested())
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    uint32 const now = getMSTime();
    if (!_deathSeenMs)
        _deathSeenMs = now;

    bool const inGrace =
        getMSTimeDiff(_deathSeenMs, now) < sPlayerbotAIConfig.resurrectWaitSeconds * IN_MILLISECONDS;

    // Resurrection range plus room for the caster to walk into it.
    constexpr float NEARBY = 40.0f;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
            member->GetExactDist(bot) > NEARBY)
            continue;

        // A cast in progress at this bot is always worth waiting for, grace period or not.
        if (Spell const* spell = member->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (spell->GetSpellInfo()->HasEffect(SPELL_EFFECT_RESURRECT) &&
                (spell->m_targets.GetUnitTargetGUID() == bot->GetGUID() ||
                 spell->m_targets.GetCorpseTargetGUID() == bot->GetGUID() ||
                 (bot->GetCorpse() && spell->m_targets.GetCorpseTargetGUID() == bot->GetCorpse()->GetGUID())))
                return true;

        if (inGrace && !member->IsInCombat() && KnowsResurrection(member))
            return true;
    }

    return false;
}

bool AutoReleaseSpiritAction::HandleBattlegroundSpiritHealer()
{
    constexpr uint32_t RESURRECT_DELAY = 15;
    const time_t now = time(nullptr);

    if ((now - m_bgGossipTime < RESURRECT_DELAY) &&
        bot->HasAura(SPELL_WAITING_FOR_RESURRECT))
    {
        return false;
    }

    float bgRange = 2000.0f;
    GuidVector npcs = NearestNpcsValue(botAI, bgRange);
    Unit* spiritHealer = nullptr;

    for (auto const& guid : npcs)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsFriendlyTo(bot) && unit->IsSpiritService())
        {
            spiritHealer = unit;
            break;
        }
    }

    if (!spiritHealer)
        return false;

    if (bot->GetDistance(spiritHealer) >= INTERACTION_DISTANCE)
    {
        // Bot needs to actually click spirit-healer in BG to get res timer going
        // and in IOC it's not within clicking range when they res in own base

        // Teleport to nearest friendly Spirit Healer when not currently in range of one.
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        bot->TeleportTo(bot->GetMapId(), spiritHealer->GetPositionX(), spiritHealer->GetPositionY(), spiritHealer->GetPositionZ(), 0.f);
        RESET_AI_VALUE(bool, "combat::self target");
        RESET_AI_VALUE(WorldPosition, "current position");
    }
    else if (!IsSelfBot(bot))
    {
        m_bgGossipTime = now;
        WorldPacket packet(CMSG_GOSSIP_HELLO);
        packet << spiritHealer->GetGUID();
        bot->GetSession()->HandleGossipHelloOpcode(packet);
    }

    return true;
}

bool AutoReleaseSpiritAction::ShouldAutoRelease() const
{
    if (!bot->GetGroup())
        return true;

    Player* groupLeader = botAI->GetGroupLeader();
    if (!groupLeader || groupLeader == bot)
        return true;

    if (!IsRealPlayer(botAI->GetMaster()))
        return true;

    if (IsRealPlayer(botAI->GetMaster()) &&
        groupLeader->GetMapId() == bot->GetMapId() &&
        bot->GetMap() &&
        (bot->GetMap()->IsRaid() || bot->GetMap()->IsDungeon()))
    {
        return false;
    }

    return ServerFacade::instance().IsDistanceGreaterThan(
        AI_VALUE2(float, "distance", "group leader"),
        sPlayerbotAIConfig.sightDistance);
}

bool AutoReleaseSpiritAction::ShouldDelayBattlegroundRelease() const
{
    // The below delays release to spirit with 6 seconds.
    // This prevents currently casted (ranged) spells to be re-directed to the died bot's ghost.

    // If the bot already is a spirit, reset release time and return true
    if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        botAI->bgReleaseAttemptTime = 0;
        return true;
    }

    // Delay release to spirit.
    const time_t now = time(nullptr);
    constexpr time_t RELEASE_DELAY = 6;

    if (botAI->bgReleaseAttemptTime == 0)
        botAI->bgReleaseAttemptTime = now;

    if (now - botAI->bgReleaseAttemptTime < RELEASE_DELAY)
        return false;

    botAI->bgReleaseAttemptTime = 0;
    return true;
}

bool RepopAction::Execute(Event /*event*/)
{
    const GraveyardStruct* graveyard = GetGrave(
        AI_VALUE(uint32, "death count") > 10 ||
        CalculateDeadTime() > 30 * MINUTE
    );

    if (!graveyard)
        return false;

    PerformGraveyardTeleport(graveyard);
    return true;
}

bool RepopAction::isUseful()
{
    return !bot->InBattleground();
}

int64 RepopAction::CalculateDeadTime() const
{
    if (Corpse* corpse = bot->GetCorpse())
        return time(nullptr) - corpse->GetGhostTime();

    return bot->isDead() ? 0 : 60 * MINUTE;
}

void RepopAction::PerformGraveyardTeleport(const GraveyardStruct* graveyard) const
{
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bot->TeleportTo(graveyard->Map, graveyard->x, graveyard->y, graveyard->z, 0.f);
    RESET_AI_VALUE(bool, "combat::self target");
    RESET_AI_VALUE(WorldPosition, "current position");
}

// SelfResurrectAction implementation for Warlock's Soulstone Resurrection/Shaman's Reincarnation
bool SelfResurrectAction::Execute(Event /*event*/)
{
    if (!bot->IsAlive() && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL))
    {
        WorldPacket packet(CMSG_SELF_RES);
        bot->GetSession()->HandleSelfResOpcode(packet);
        return true;
    }
    return false;
}
bool SelfResurrectAction::isUseful()
{
    return !bot->IsAlive() && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL);
}
