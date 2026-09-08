/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotRepairMgr.h"

#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringFormat.h"

namespace
{
constexpr uint32 REPAIR_INTERVAL_MS = 60 * 1000;

/// Free repairs kick in well before anything is at risk of breaking, so a bot nobody is paying
/// attention to never fights at reduced effectiveness.
constexpr float FREE_REPAIR_AT = 0.90f;
}  // namespace

float BotRepairMgr::DurabilityFraction(Player* bot)
{
    uint32 current = 0;
    uint32 maximum = 0;

    // Equipped only. A self bot should not be charged for the bags in its pack wearing out, and it
    // is equipped durability that decides whether it can fight.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        uint32 const max = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (!max)
            continue;

        current += item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        maximum += max;
    }

    if (!maximum)
        return 1.0f;

    return float(current) / float(maximum);
}

bool BotRepairMgr::PaysForRepairs(Player* bot)
{
    return IsSelfBot(bot);
}

void BotRepairMgr::Update(Player* bot, uint32 diff)
{
    if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        return;

    ObjectGuid const guid = bot->GetGUID();

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        uint32& timer = _timers[guid];

        if (timer > diff)
        {
            timer -= diff;
            return;
        }

        timer = REPAIR_INTERVAL_MS;
    }

    bool const pays = PaysForRepairs(bot);
    float const threshold = pays ? sPlayerbotAIConfig.selfBotRepairThreshold : FREE_REPAIR_AT;
    float const fraction = DurabilityFraction(bot);

    if (fraction >= threshold)
        return;

    if (!pays)
    {
        // discountMod 1.0 and cost false: nothing is charged, so the discount is irrelevant.
        bot->DurabilityRepairAll(false, 1.0f, false);

        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_freeRepairs;
        return;
    }

    // A self bot repairs from wherever it is standing. DurabilityRepairAll does not need a vendor --
    // that requirement lives in the callers that represent walking up to one -- so the cost is kept
    // and the errand is not.
    uint32 const before = bot->GetMoney();
    uint32 const spent = bot->DurabilityRepairAll(true, 1.0f, false);

    std::unique_lock<std::shared_mutex> guard(_mutex);

    if (!spent && before < 1)
    {
        ++_deferredNoGold;
        return;
    }

    if (spent)
    {
        ++_paidRepairs;
        _goldSpent += spent;
        LOG_DEBUG("playerbots", "[Repair] {} repaired at {:.0f}% durability for {}c", bot->GetName(), fraction * 100.0f,
                  spent);
    }
    else
    {
        // Below the threshold but nothing was charged: not enough gold to cover any single item.
        ++_deferredNoGold;
        LOG_DEBUG("playerbots", "[Repair] {} needs repairs at {:.0f}% but cannot afford them ({}c)", bot->GetName(),
                  fraction * 100.0f, before);
    }
}

std::string BotRepairMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("repairs: {} free, {} paid for {}c total, {} deferred for lack of gold", _freeRepairs,
                               _paidRepairs, _goldSpent, _deferredNoGold);
}
