/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTAGENDAMGR_H
#define PLAYERBOTS_BOTAGENDAMGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include "PlayerbotAIConfig.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

/**
 * What a bot is trying to achieve, as opposed to what it is doing right now.
 *
 * `NewRpgInfo` holds the current activity: one slot, one thing at a time. Nothing above it holds
 * intent, so every bot on the realm draws its next activity from the same global weight table and
 * they all behave identically in aggregate. A realm of three thousand bots that all want the same
 * thing does not read as a world; it reads as one bot copied three thousand times.
 */
enum class BotGoalType : uint8
{
    LevelUp = 0,
    EarnGold = 1,
    AcquireGear = 2,
    TrainProfession = 3,
    RestockConsumables = 4,
    ClearInventory = 5,
    Socialise = 6,

    Max = 7
};

/**
 * A bot's persistent disposition.
 *
 * Goals converge: given enough time every bot wants gold, levels and a trained profession, so a
 * realm driven by goals alone drifts back toward uniformity. An archetype is fixed for the life of
 * the character and biases the baseline, so a Gatherer and a Socialite with identical goals still
 * spend their days differently.
 */
enum class BotArchetype : uint8
{
    Questor = 0,
    Gatherer = 1,
    Grinder = 2,
    Trader = 3,
    Socialite = 4,
    PvPer = 5,

    Max = 6
};

struct BotGoal
{
    BotGoalType type{BotGoalType::LevelUp};
    uint32 param{0};      ///< skill id, slot mask -- meaning depends on type
    int64 target{0};      ///< gold amount, skill value, level
    int64 progress{0};
    uint32 createdAt{0};
    uint32 expiresAt{0};  ///< 0 = never
    uint8 priority{0};    ///< 0-255, recomputed every agenda tick

    /// Derived goals are recomputed from the character and never written to the database.
    [[nodiscard]] bool IsDerived() const
    {
        return type == BotGoalType::ClearInventory || type == BotGoalType::LevelUp;
    }
};

/**
 * Per-bot agendas, and the weighting they impose on activity selection.
 *
 * Evaluation is round-robin with a fixed budget per tick rather than "every bot every tick", so the
 * cost is flat as the bot count rises. A realm with 3000 bots does the same work per tick as one
 * with 300; individual bots simply have their goals revisited less often, which is fine because a
 * goal is a thing that changes over minutes, not milliseconds.
 */
class BotAgendaMgr
{
public:
    static BotAgendaMgr& instance()
    {
        static BotAgendaMgr instance;
        return instance;
    }

    void Load();

    /// Round-robin agenda evaluation. World thread.
    void Update(uint32 diff);

    /**
     * Multiplier this bot's goals impose on one activity, around 1.0.
     *
     * Linear rather than multiplicative: each goal contributes `(affinity - 1) x priority`, and the
     * contributions are summed. Multiplying them would let three mildly-interested goals swamp the
     * base weights entirely, which is how a scoring system stops being tunable.
     */
    float GetActivityMultiplier(Player* bot, NewRpgStatus status) const;

    /**
     * A weight this bot's archetype insists on, overriding the global table, or 0 for none.
     *
     * Needed because a global weight of 0 is not a small number, it is off: the selector drops a
     * zero-weight status before the multiplier is applied, so no archetype affinity can bring it
     * back. Socialites wandering to NPCs is exactly that case -- the activity is disabled realm
     * wide and enabled for the one disposition it defines.
     */
    int32 GetActivityBaseOverride(Player* bot, NewRpgStatus status) const;

    /// Human-readable dump for `.playerbots agenda <name>`.
    std::string DescribeAgenda(Player* bot) const;

    /// This bot's archetype, assigning one on first use.
    BotArchetype GetArchetype(Player* bot);

    /// Name of this bot's archetype, or "none" if it has not been assigned yet. Read-only.
    char const* GetArchetypeLabel(Player* bot) const;

    /// Realm-wide archetype distribution, for `.playerbots archetypes`.
    std::string DescribeDistribution() const;

    /// Drop a bot's cached agenda, e.g. on logout.
    void Forget(ObjectGuid guid);

private:
    BotAgendaMgr() = default;
    ~BotAgendaMgr() = default;

    BotAgendaMgr(BotAgendaMgr const&) = delete;
    BotAgendaMgr& operator=(BotAgendaMgr const&) = delete;

    using Agenda = std::vector<BotGoal>;

    /// Recompute derived goals and re-prioritise stored ones for a single bot.
    void Evaluate(Player* bot);

    /// Persist a bot's non-derived goals.
    void Save(ObjectGuid guid, Agenda const& agenda);

    /// Draw an archetype from the configured shares.
    static BotArchetype RollArchetype();

    mutable std::shared_mutex _mutex;
    std::unordered_map<ObjectGuid, Agenda> _agendas;
    std::unordered_map<ObjectGuid, BotArchetype> _archetypes;

    // Round-robin cursor into the online bot list, so each tick advances rather than restarting.
    uint32 _cursor{0};
    uint32 _timer{0};
};

#define sBotAgendaMgr BotAgendaMgr::instance()

#endif
