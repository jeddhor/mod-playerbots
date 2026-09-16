/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotConsumableMgr.h"

#include "BotEconomyMgr.h"
#include "ChatHelper.h"
#include "Item.h"
#include "Bag.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <vector>

namespace
{
constexpr uint32 CONSUMABLE_INTERVAL_MS = 60 * 1000;

/// Long enough to be a buff rather than an instant effect. Below this it is a heal, not a blessing.
constexpr uint32 MIN_BUFF_DURATION_MS = 60 * 1000;
}  // namespace

bool BotConsumableMgr::InSeriousContent(Player* bot)
{
    if (bot->InBattleground() || bot->InArena())
        return true;

    Map const* map = bot->FindMap();
    return map && (map->IsDungeon() || map->IsRaid());
}

SpellInfo const* BotConsumableMgr::BuffSpellOf(Item* item) const
{
    ItemTemplate const* proto = item->GetTemplate();
    if (proto->Class != ITEM_CLASS_CONSUMABLE)
        return nullptr;

    switch (proto->SubClass)
    {
        case ITEM_SUBCLASS_POTION:
        case ITEM_SUBCLASS_ELIXIR:
        case ITEM_SUBCLASS_FLASK:
        case ITEM_SUBCLASS_SCROLL:
            break;
        default:
            return nullptr;
    }

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (proto->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE || !proto->Spells[i].SpellId)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(proto->Spells[i].SpellId);
        if (!info)
            continue;

        // A buff, not a heal. Healing and mana potions are the UsePotions strategy's business and it
        // decides by health, which is the right test for them and the wrong one here.
        if (info->GetDuration() < int32(MIN_BUFF_DURATION_MS))
            continue;

        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            if (info->Effects[e].Effect == SPELL_EFFECT_APPLY_AURA)
                return info;
    }

    return nullptr;
}

bool BotConsumableMgr::ConflictsWithHeldBuffs(Player* bot, SpellInfo const* info) const
{
    // Already on. Re-drinking would refresh a duration at the price of a whole item.
    if (bot->HasAura(info->Id))
        return true;

    // The core's own stack tables answer the rest: battle elixir against guardian elixir, either
    // against a flask, a scroll against the elixir that shares its group. Anything that would be
    // refused, or would push off what the bot is already carrying, is not worth the item.
    for (auto const& [auraId, aura] : bot->GetOwnedAuras())
    {
        if (!aura)
            continue;

        SpellInfo const* held = aura->GetSpellInfo();
        if (!held || held->Id == info->Id)
            continue;

        switch (sSpellMgr->CheckSpellGroupStackRules(info, held))
        {
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE:
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER:
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT:
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST:
                return true;
            default:
                break;
        }
    }

    return false;
}

bool BotConsumableMgr::IsPremium(Item* item) const
{
    ItemTemplate const* proto = item->GetTemplate();

    // A flask is the definition of the expensive case, whatever the market says about it today.
    if (proto->SubClass == ITEM_SUBCLASS_FLASK)
        return true;

    // Everything else by what it is actually worth. Market price rather than vendor price, because
    // vendor price says almost nothing about a crafted consumable -- the ones worth husbanding are
    // expensive because other people want them.
    return sBotEconomyMgr.GetMarketPrice(proto->ItemId) > sPlayerbotAIConfig.consumablePremiumValue;
}

void BotConsumableMgr::Update(Player* bot, uint32 diff)
{
    if (!sPlayerbotAIConfig.consumablesEnabled || !bot || !bot->IsInWorld())
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    // Buffs go on before the fight, not during it: mid-combat item use is the potion strategy's
    // job. Also never while the owner is steering, mounted, dead, or being moved between maps.
    if (bot->IsInCombat() || bot->isDead() || bot->IsMounted() || bot->IsBeingTeleported() ||
        botAI->HumanIsDriving())
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

        timer = CONSUMABLE_INTERVAL_MS;
    }

    bool const serious = InSeriousContent(bot);

    // The bags, walked directly: IterateItems belongs to the action classes and this is a manager.
    std::vector<Item*> candidates;
    auto collect = [&candidates](Item* item)
    {
        if (item)
            candidates.push_back(item);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        collect(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                collect(bag->GetItemByPos(uint8(slot)));

    for (Item* item : candidates)
    {
        uint32 const entry = item->GetTemplate()->ItemId;

        SpellInfo const* info = BuffSpellOf(item);
        if (!info)
            continue;

        if (bot->CanUseItem(item->GetTemplate()) != EQUIP_ERR_OK || bot->HasSpellCooldown(info->Id))
            continue;

        // Read before the item is drunk: using it destroys the stack's last item, and everything
        // below would then be asking questions of a freed pointer.
        bool const premium = IsPremium(item);
        std::string const name = item->GetTemplate()->Name1;

        if (premium && sPlayerbotAIConfig.consumablePremiumInSeriousContentOnly && !serious)
        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_skippedPremium;
            continue;
        }

        if (ConflictsWithHeldBuffs(bot, info))
        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_skippedConflict;
            continue;
        }

        // Through the existing use action rather than casting here: it owns the "can this item be
        // used at all" rules, the cast, and the bot's own book-keeping about what it just drank.
        if (!botAI->DoSpecificAction("use", Event("consumables", ChatHelper::FormatQItem(entry)), true))
            continue;

        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            ++_used;
            if (premium)
                ++_premiumUsed;
        }

        LOG_DEBUG("playerbots", "[Consumable] {} drank {} ({}){}", bot->GetName(), name, info->Id,
                  serious ? " in instanced content" : "");

        // One per pass. Buffs last minutes and the next pass is a minute away, so a bot works
        // through its stat buffs one at a time rather than emptying its bags at a zone entrance.
        return;
    }
}

std::string BotConsumableMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat(
        "consumables: {} used ({} premium), {} skipped as already buffed, {} premium held back",
        _used, _premiumUsed, _skippedConflict, _skippedPremium);
}
