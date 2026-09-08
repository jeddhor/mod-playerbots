/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTCRAFTMGR_H
#define PLAYERBOTS_BOTCRAFTMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class Player;

/**
 * Bots craft the things other bots' professions depend on, and sell them.
 *
 * Enchanting is the case that forces this. An enchanter needs a runed rod, no vendor sells one, and
 * an enchanter cannot make the blank it is runed from -- a blacksmith makes that. So an enchanter on
 * a realm of bots is stuck at the first rod unless some blacksmith decides to supply the market.
 *
 * What counts as a supply item is derived rather than listed. A tool is any item with a
 * TotemCategory; the reagents of the spells that create tools are therefore the blanks the tool
 * chain depends on. That reaches every rod tier without naming one, and would reach a tool added by
 * a later patch on its own.
 *
 * Supply is demand-led: a bot crafts a blank only when the auction house is short of it, so this
 * does not turn every blacksmith into a rod factory flooding the market with copper rods.
 */
class BotCraftMgr
{
public:
    static BotCraftMgr& instance()
    {
        static BotCraftMgr instance;
        return instance;
    }

    /// Per-bot tick. Cheap except on the periodic pass.
    void Update(Player* bot, uint32 diff);

    /// True if this item is a reagent of some tool-creating recipe, and so worth stocking.
    bool IsToolBlank(uint32 itemId);

    std::string DescribeStats() const;

private:
    BotCraftMgr() = default;
    ~BotCraftMgr() = default;

    BotCraftMgr(BotCraftMgr const&) = delete;
    BotCraftMgr& operator=(BotCraftMgr const&) = delete;

    /// Walk every spell once to find which items are tools, and what those tools are made from.
    void EnsureLoaded();

    /// Craft one unit of the item this spell makes, consuming its reagents. False if short.
    bool CraftOne(Player* bot, uint32 spellId, uint32 itemId);

    std::once_flag _loadOnce;

    /// Items that are reagents of a tool recipe -- rod blanks and their equivalents.
    std::unordered_set<uint32> _toolBlanks;

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, uint32> _timers;

    uint32 _crafted{0};
    uint32 _listed{0};
    uint32 _shortReagents{0};
};

#define sBotCraftMgr BotCraftMgr::instance()

#endif
