/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Ported with permission from Dustin HendricksonBased from mod-player-bot-level-brackets
 * and mod-player-bot-reset modules with contributors NoxMax (level reset), jimm0thy (friend-list exclusion),
 * Jered Little (arena-team exclusion).
 */

#ifndef PLAYERBOTS_RANDOMBOTLEVELMGR_H
#define PLAYERBOTS_RANDOMBOTLEVELMGR_H

#include "ObjectGuid.h"
#include "PlayerbotAIConfig.h"
#include "SharedDefines.h"
#include <string>
#include <vector>

class Player;

// Owns two ported sub-features: periodic redistribution of random bots across per-faction level
// brackets, and resetting random bots that reach max level. Config lives in PlayerbotAIConfig;
// this class holds the runtime working state (working bracket copies, pending-reset queue, timers).
class RandomBotLevelMgr
{
public:
    static RandomBotLevelMgr& instance()
    {
        static RandomBotLevelMgr instance;

        return instance;
    }

    /**
     * The expansion ceiling this bot stops at: 60, 70, or 0 for a bot that levels to the cap.
     *
     * Derived from the character's GUID rather than rolled or stored. A roll re-taken at each login
     * would move bots in and out of the capped population every restart, which is precisely the
     * drift the phase is meant to avoid; a stored column would need a table, a migration and a
     * repair path for bots created before it existed. A GUID is already stable, already unique, and
     * already loaded.
     *
     * The consequence worth knowing: which bots are capped is fixed for the life of the character,
     * so changing the percentages moves the boundary rather than reshuffling the roster.
     */
    static uint8 EraCapFor(Player* bot);

    /**
     * Set or clear PLAYER_FLAGS_NO_XP_GAIN according to the bot's era cap.
     *
     * Must be called from every place that touches the flag. Both existing sites cleared it
     * unconditionally whenever RandomBotFixedLevel was off, so an era cap applied anywhere else
     * would have been stripped again at the bot's next login.
     */
    static void ApplyXpGainPolicy(Player* bot);

    /// One report for .rndbot eras: how many bots are assigned to each ceiling and how many have
    /// actually reached it. The phase's acceptance test is that these numbers stop moving.
    static std::string DescribeEraPopulation();

    void LoadConfig();
    void LogStartupSummary() const;
    void Update(uint32 diff);
    void OnBotLogin(Player* player);
    void OnBotLevelChanged(Player* player, uint8 oldLevel);
    void OnPlayerLogout(Player* player);

private:
    RandomBotLevelMgr() = default;
    ~RandomBotLevelMgr() = default;

    RandomBotLevelMgr(RandomBotLevelMgr const&) = delete;
    RandomBotLevelMgr& operator=(RandomBotLevelMgr const&) = delete;

    RandomBotLevelMgr(RandomBotLevelMgr&&) = delete;
    RandomBotLevelMgr& operator=(RandomBotLevelMgr&&) = delete;

    // Resolved back to the live _allianceRanges/_hordeRanges vector at process time, since those
    // vectors can be resized on a config reload.
    struct PendingResetEntry
    {
        ObjectGuid botGuid;
        int targetRange;
        TeamId team;
    };

    // ---- Level brackets sub-feature ----
    std::vector<LevelBracketConfig>& GetFactionRanges(TeamId team);
    void ClampAndBalanceBrackets();
    void ApplyBracketWeights(std::vector<LevelBracketConfig>& ranges, std::vector<float> const& weights);
    int GetLevelRangeIndex(uint8 level, TeamId team);
    void AdjustBotToRange(Player* bot, int targetRangeIndex, TeamId team);
    void LoadSocialFriendList();
    int GetOrFlagPlayerBracket(Player* player);
    void RunLevelBracketsDistribution();
    void ProcessFactionDistribution(TeamId team, uint32 totalBots, std::vector<int>& actualCounts,
        std::vector<std::vector<Player*>>& botsByRange);
    void RedistributeSurplusBots(std::vector<Player*>& sourceBots, int fromRange, TeamId team,
        std::vector<int>& actualCounts, std::vector<int> const& desiredCounts, std::vector<int> const& targetRanges);
    void ProcessPendingLevelResets();

    /// Bring bots that sit above their era ceiling down onto it, a few at a time.
    void RunEraSeedingPass();

    /// Bring bots already parked on their ceiling up to the gear their era expects. The seeding pass only
    /// touches bots whose level is wrong, so without this the standing population keeps whatever it was
    /// wearing when it arrived -- which is rare gear, because that is all the ordinary gear path hands out.
    void RunEraRegearPass();

    /// The gear an era ceiling is dressed for: quality, and an item level ceiling.
    static void EraGearTarget(uint8 cap, uint32& quality, uint32& ilvl);

    // ---- Level reset sub-feature ----
    uint8 ComputeResetChance(uint8 level) const;
    void ResetBot(Player* player, uint8 currentLevel);
    void SkipBotLevel(Player* player, uint8 currentLevel);
    void RunResetPlayedTimeCheck();

    // Level brackets: working copies, since dynamic distribution and the clamp/rebalance pass
    // mutate percentages at runtime and must never write back into PlayerbotAIConfig.
    std::vector<LevelBracketConfig> _allianceRanges;
    std::vector<LevelBracketConfig> _hordeRanges;
    uint8 _numRanges = 9;
    uint8 _randomBotMinLevel = 1;
    uint8 _randomBotMaxLevel = 80;
    std::vector<PendingResetEntry> _pendingLevelResets;
    std::vector<uint32> _socialFriendsList;

    uint32 _bracketsTimer = 0; // Level brackets: distribution adjustments
    uint32 _flaggedTimer = 0;  // Level brackets: pending reset checks
    uint32 _resetTimer = 0;    // Level reset: played-time based reset checks
    uint32 _eraSeedTimer = 0;  // Era caps: bringing bots above their ceiling down onto it
    uint32 _eraSeeded = 0;
    uint32 _eraRegeared = 0;
};

// Registers the random bot level brackets + level reset world/player scripts.
void AddSC_randombot_level_mgr();

#endif // PLAYERBOTS_RANDOMBOTLEVELMGR_H
