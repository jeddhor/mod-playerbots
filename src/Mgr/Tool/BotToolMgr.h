/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTTOOLMGR_H
#define PLAYERBOTS_BOTTOOLMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class Player;

/**
 * Keeps bots carrying the tools their professions need.
 *
 * A miner without a pick cannot mine, and nothing in the bot AI ever noticed. Rather than hardcode
 * "miners need item 2901", this follows the same data the client and the core use: a recipe names a
 * TotemCategory it requires, an item declares the category it satisfies, and Player already knows
 * how to answer whether it is holding something suitable. Every tool in the game is covered by that
 * one rule -- picks, skinning knives, blacksmith hammers, inking sets, enchanting rods -- and a tool
 * added by a future patch is covered without touching this file.
 *
 * Vendor-sold tools are bought where the bot stands, for the same reason training is: the errand is
 * not the interesting part, the expense is. Tools no vendor sells -- every runed rod, the gyromatic
 * micro-adjustor -- are left alone here; those have to be crafted or bought from other players, and
 * that is the auction-house half of the problem.
 */
class BotToolMgr
{
public:
    static BotToolMgr& instance()
    {
        static BotToolMgr instance;
        return instance;
    }

    /// Per-bot tick. Cheap except on the periodic pass.
    void Update(Player* bot, uint32 diff);

    /// Categories this bot's known recipes require but which nothing it carries satisfies.
    std::unordered_set<uint32> MissingToolCategories(Player* bot);

    /// True if this item is a tool one of the bot's own recipes requires.
    bool IsNeededTool(Player* bot, uint32 totemCategory);

    /// The cheapest vendor-sold item satisfying a category, or 0 if no vendor stocks one.
    uint32 VendorToolFor(uint32 totemCategory);

    /// Whether the bot has a fishing pole equipped or anywhere in its bags. Fishing needs one, and
    /// unlike a mining pick it is not a totem category, so the category test above never sees it.
    static bool HasFishingPole(Player* bot);

    std::string DescribeStats() const;

private:
    BotToolMgr() = default;
    ~BotToolMgr() = default;

    BotToolMgr(BotToolMgr const&) = delete;
    BotToolMgr& operator=(BotToolMgr const&) = delete;

    /// Index vendor stock and tool items once, on first use.
    void EnsureLoaded();

    std::once_flag _loadOnce;

    /// totem category -> cheapest item a vendor actually stocks, with its price.
    std::unordered_map<uint32, std::pair<uint32, uint32>> _vendorTools;

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, uint32> _timers;

    uint32 _bought{0};
    uint64 _spent{0};
    uint32 _unaffordable{0};
    uint32 _noVendor{0};
};

#define sBotToolMgr BotToolMgr::instance()

#endif
