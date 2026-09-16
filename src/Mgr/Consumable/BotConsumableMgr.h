/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTCONSUMABLEMGR_H
#define PLAYERBOTS_BOTCONSUMABLEMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>

class Item;
class Player;
class SpellInfo;

/**
 * Bots drink their own buffs.
 *
 * A bot that carries Scrolls of Strength, elixirs and flasks and never touches them is carrying
 * dead weight -- and the operator's own alts arrive at a dungeon unbuffed while their bags hold
 * exactly what they should have drunk. Emergency healing and mana potions already had a strategy;
 * nothing ever applied a stat buff.
 *
 * Two rules shape it.
 *
 * **Never overwrite something better.** Battle and guardian elixirs each occupy their own slot, a
 * flask replaces both, and scroll buffs share a group with some of them. Rather than encode any of
 * that here, every candidate is checked against the auras the bot already has using the core's own
 * spell-group stack rules -- the same tables that stop a player's elixir replacing their flask. A
 * buff that would be refused, or would knock off something the bot is already carrying, is skipped.
 *
 * **The expensive ones are for content that warrants them.** A flask costs a raid's worth of
 * materials; drinking it to quest in Elwynn is how a bot burns an operator's gold. Flasks and
 * anything above a configurable market value are used only in dungeons, raids and battlegrounds.
 */
class BotConsumableMgr
{
public:
    static BotConsumableMgr& instance()
    {
        static BotConsumableMgr instance;
        return instance;
    }

    /// Per-bot tick. Cheap except on the periodic pass.
    void Update(Player* bot, uint32 diff);

    std::string DescribeStats() const;

private:
    BotConsumableMgr() = default;
    ~BotConsumableMgr() = default;

    BotConsumableMgr(BotConsumableMgr const&) = delete;
    BotConsumableMgr& operator=(BotConsumableMgr const&) = delete;

    /// The on-use spell of a consumable that applies a lasting buff, or nullptr if it is not one.
    SpellInfo const* BuffSpellOf(Item* item) const;

    /// True if drinking this would be wasted: already held, or refused/overwriting what is held.
    bool ConflictsWithHeldBuffs(Player* bot, SpellInfo const* info) const;

    /// True if this is one of the expensive ones, kept for instanced content.
    bool IsPremium(Item* item) const;

    /// Dungeon, raid or battleground -- where the expensive ones are worth drinking.
    static bool InSeriousContent(Player* bot);

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, uint32> _timers;

    uint32 _used{0};
    uint32 _premiumUsed{0};
    uint32 _skippedConflict{0};
    uint32 _skippedPremium{0};
};

#define sBotConsumableMgr BotConsumableMgr::instance()

#endif
