/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LOOTOBJECTSTACK_H
#define PLAYERBOTS_LOOTOBJECTSTACK_H

#include "ObjectGuid.h"

#include <ctime>
#include <unordered_map>

class AiObjectContext;
class Player;
class WorldObject;

struct ItemTemplate;

class LootStrategy
{
public:
    LootStrategy() {}
    virtual ~LootStrategy(){};
    virtual bool CanLoot(ItemTemplate const* proto, AiObjectContext* context) = 0;
    virtual std::string const GetName() = 0;
};

class LootObject
{
public:
    LootObject() : skillId(0), reqSkillValue(0), reqItem(0) {}
    LootObject(Player* bot, ObjectGuid guid);
    LootObject(LootObject const& other);
    LootObject& operator=(LootObject const& other) = default;

    bool IsEmpty() { return !guid; }
    bool IsLootPossible(Player* bot);
    void Refresh(Player* bot, ObjectGuid guid);
    WorldObject* GetWorldObject(Player* bot);
    ObjectGuid guid;

    uint32 skillId;
    uint32 reqSkillValue;
    uint32 reqItem;

private:
    static bool IsNeededForQuest(Player* bot, uint32 itemId);
};

class LootTarget
{
public:
    LootTarget(ObjectGuid guid);
    LootTarget(LootTarget const& other);

public:
    LootTarget& operator=(LootTarget const& other);
    bool operator<(LootTarget const& other) const;

public:
    ObjectGuid guid;
    time_t asOfTime;
};

class LootTargetList : public std::set<LootTarget>
{
public:
    void shrink(time_t fromTime);
};

class LootObjectStack
{
public:
    LootObjectStack(Player* bot) : bot(bot) {}

    bool Add(ObjectGuid guid);
    void Remove(ObjectGuid guid);
    void Clear();
    bool CanLoot(float maxDistance);
    LootObject GetLoot(float maxDistance = 0);

    /**
     * Loot was left behind in this object, so stop offering it for a while.
     *
     * A mining node keeps its loot until somebody takes all of it, and a bot that declines one of
     * the items -- bags near full, a stack it cannot start, a unique it already owns -- leaves the
     * node lootable. The gather target picker then hands back the same node, forever. Observed
     * directly: a bot in Jasperlode Mine topping up its Rough Stone from one copper vein over and
     * over while never taking the ore, because it had a part-used Rough Stone stack and no ore
     * stack at all.
     *
     * Remembering the failure is the fix rather than forcing the loot: whatever stopped the bot
     * taking the item is still true a second later, so retrying immediately cannot succeed, and
     * only the loop is worth breaking here.
     */
    void MarkUnfinished(ObjectGuid guid);
    bool IsUnfinished(ObjectGuid guid) const;

private:
    LootObject GetNearest(float maxDistance = 0);

    Player* bot;
    LootTargetList availableLoot;
    /// guid -> the time after which it may be offered again.
    std::unordered_map<ObjectGuid, time_t> unfinished;
};

#endif
