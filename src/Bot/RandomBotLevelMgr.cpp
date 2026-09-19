/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Ported with permission from Dustin HendricksonBased from mod-player-bot-level-brackets
 * and mod-player-bot-reset modules with contributors NoxMax (level reset), jimm0thy (friend-list exclusion),
 * Jered Little (arena-team exclusion).
 */

#include "RandomBotLevelMgr.h"
#include "DatabaseEnv.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "QueryResult.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

// True if bot's name is present in excludeList.
static bool IsNameInExcludeList(Player* bot, std::vector<std::string> const& excludeList)
{
    if (!bot)
        return false;

    return std::find(excludeList.begin(), excludeList.end(), bot->GetName()) != excludeList.end();
}

// Checks if the given bot is present in any real player's friends list.
static bool BotInFriendList(Player* bot, std::vector<uint32> const& socialFriendsList)
{
    if (!bot || !bot->IsInWorld() || !bot->GetSession() || bot->GetSession()->isLogingOut() ||
        bot->IsDuringRemoveFromWorld())
        return false;

    return std::find(socialFriendsList.begin(), socialFriendsList.end(), bot->GetGUID().GetCounter()) !=
        socialFriendsList.end();
}

// Checks if the given bot is a member of any arena team.
static bool BotInArenaTeam(Player* bot)
{
    if (!bot)
        return false;
    for (uint32 slot = 0; slot < MAX_ARENA_SLOT; ++slot)
    {
        if (bot->GetArenaTeamId(slot))
            return true;
    }
    return false;
}

// Checks if a bot is currently in a safe state to perform a level reset (alive, not in combat, not
// in a battleground/arena/dungeon queue or flight, and grouped only with other bots).
static bool IsBotSafeForLevelReset(Player* bot)
{
    if (!bot || !bot->GetSession() || bot->GetSession()->isLogingOut() || bot->IsDuringRemoveFromWorld())
        return false;

    if (!bot->IsInWorld())
        return false;

    if (!bot->IsAlive())
        return false;

    if (bot->IsInCombat())
        return false;

    if (bot->InBattleground() || bot->InArena() || bot->inRandomLfgDungeon() || bot->InBattlegroundQueue())
        return false;

    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        return false;

    if (Group* group = bot->GetGroup())
    {
        if (sLFGMgr->GetState(group->GetGUID()) != lfg::LFG_STATE_NONE)
            return false;
    }

    if (bot->IsInFlight())
        return false;

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && !GET_PLAYERBOT_AI(member))
                return false;
        }
    }
    return true;
}

// =============================================================================
// LEVEL BRACKETS FEATURE
// =============================================================================

std::vector<LevelBracketConfig>& RandomBotLevelMgr::GetFactionRanges(TeamId team)
{
    return (team == TEAM_ALLIANCE) ? _allianceRanges : _hordeRanges;
}

// Copies the bracket definitions from PlayerbotAIConfig into the working state and resets the
// working bounds/percentages. Dynamic distribution and the clamp/rebalance pass below mutate these
// working copies at runtime, so PlayerbotAIConfig's own vectors are never touched after this point.
namespace
{
    /// The three expansion ceilings. Not configurable: they are facts about the game, not policy.
    constexpr uint8 ERA_CAP_CLASSIC = 60;
    constexpr uint8 ERA_CAP_TBC = 70;
    constexpr uint8 ERA_CAP_WOTLK = 80;
}

uint8 RandomBotLevelMgr::EraCapFor(Player* bot)
{
    if (!bot)
        return 0;

    uint32 const pct60 = sPlayerbotAIConfig.eraCappedBotPctAt60;
    uint32 const pct70 = sPlayerbotAIConfig.eraCappedBotPctAt70;
    uint32 const pct80 = sPlayerbotAIConfig.eraCappedBotPctAt80;
    if (!pct60 && !pct70 && !pct80)
        return 0;

    // The GUID counter is dense and sequential, so taking it modulo 100 spreads bots evenly across
    // the hundred slots without needing a hash.
    uint32 const slot = bot->GetGUID().GetCounter() % 100;

    if (slot < pct60)
        return ERA_CAP_CLASSIC;
    if (slot < pct60 + pct70)
        return ERA_CAP_TBC;
    if (slot < pct60 + pct70 + pct80)
        return ERA_CAP_WOTLK;

    return 0;
}

void RandomBotLevelMgr::ApplyXpGainPolicy(Player* bot)
{
    if (!bot)
        return;

    // Era caps shape the random bot population. A person's own character -- their alt bots and
    // their self bot -- levels when they level, whatever the roster is meant to look like.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    if (sPlayerbotAIConfig.randomBotFixedLevel)
    {
        bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
        return;
    }

    uint8 const cap = EraCapFor(bot);

    // Exactly at its ceiling: stop here, and stay here. The played-time reset skips a bot sitting on
    // its ceiling, so a parked bot is never recycled and the population holds -- which is what a raid
    // roster needs, and it is why the level 80 era can sit on the level cap at all.
    //
    // Deliberately not ">= cap". A bot already past its ceiling is left alone rather than frozen
    // wherever it happens to be: freezing a level 75 bot with a 70 cap would strand it in a level
    // no reset path ever looks at, so it could never come back round to its era. Left alone it
    // reaches the cap, is recycled by played time, and stops at 70 on the way back up.
    if (cap && bot->GetLevel() == cap)
    {
        if (!bot->HasPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN))
            LOG_INFO("playerbots", "[Era] {} has reached the level {} ceiling and stops there",
                     bot->GetName(), cap);

        bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
        return;
    }

    bot->RemovePlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
}

std::string RandomBotLevelMgr::DescribeEraPopulation()
{
    uint32 assigned60 = 0, assigned70 = 0, assigned80 = 0, parked60 = 0, parked70 = 0, parked80 = 0;
    uint32 uncapped = 0, belowCap = 0;

    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsInWorld() || !sRandomPlayerbotMgr.IsRandomBot(bot))
            continue;

        uint8 const cap = EraCapFor(bot);
        if (!cap)
        {
            ++uncapped;
            continue;
        }

        uint32& assigned = cap == 60 ? assigned60 : (cap == 70 ? assigned70 : assigned80);
        uint32& parked = cap == 60 ? parked60 : (cap == 70 ? parked70 : parked80);
        ++assigned;

        if (bot->GetLevel() == cap)
            ++parked;
        else if (bot->GetLevel() < cap)
            ++belowCap;
    }

    return Acore::StringFormat(
        "eras: configured {}% at 60, {}% at 70 and {}% at 80.\n"
        "  online and assigned: {} to 60 ({} there now), {} to 70 ({} there now), {} to 80 ({} there "
        "now), {} uncapped.\n"
        "  {} assigned bots are not on their ceiling yet, {} have been moved onto one, {} re-geared for their era.\n"
        "Assigned counts follow the config immediately; the 'there now' counts fill in as the seeding "
        "pass moves bots onto their ceiling, then should stop moving. A 'there now' figure that keeps "
        "falling means something is still pulling capped bots out of their era.",
        sPlayerbotAIConfig.eraCappedBotPctAt60, sPlayerbotAIConfig.eraCappedBotPctAt70,
        sPlayerbotAIConfig.eraCappedBotPctAt80, assigned60, parked60, assigned70, parked70, assigned80, parked80,
        uncapped, belowCap, instance()._eraSeeded, instance()._eraRegeared);
}

void RandomBotLevelMgr::LoadConfig()
{
    _allianceRanges = sPlayerbotAIConfig.levelBracketsAlliance;
    _hordeRanges = sPlayerbotAIConfig.levelBracketsHorde;
    _numRanges = sPlayerbotAIConfig.levelBracketsNumRanges;
    _randomBotMinLevel = static_cast<uint8>(sPlayerbotAIConfig.randomBotMinLevel);
    _randomBotMaxLevel = static_cast<uint8>(sPlayerbotAIConfig.randomBotMaxLevel);

    ClampAndBalanceBrackets();
}

void RandomBotLevelMgr::LogStartupSummary() const
{
    if (!sPlayerbotAIConfig.levelBracketsEnabled)
        LOG_INFO("playerbots", "[RandomBotLevelMgr] Level brackets sub-feature disabled via configuration.");
    else
    {
        LOG_DEBUG("playerbots",
            "[RandomBotLevelMgr] Level brackets loaded. Check frequency: {} seconds, flagged check frequency: {} "
            "seconds.",
            sPlayerbotAIConfig.levelBracketsCheckFrequency, sPlayerbotAIConfig.levelBracketsFlaggedCheckFrequency);
        for (uint8 i = 0; i < _numRanges; ++i)
            LOG_DEBUG("playerbots", "[RandomBotLevelMgr] Alliance Range {}: {}-{}, Desired Percentage: {}%", i + 1,
                _allianceRanges[i].lower, _allianceRanges[i].upper, _allianceRanges[i].pct);
        for (uint8 i = 0; i < _numRanges; ++i)
            LOG_DEBUG("playerbots", "[RandomBotLevelMgr] Horde Range {}: {}-{}, Desired Percentage: {}%", i + 1,
                _hordeRanges[i].lower, _hordeRanges[i].upper, _hordeRanges[i].pct);
    }

    if (!sPlayerbotAIConfig.eraCappedBotPctAt60 && !sPlayerbotAIConfig.eraCappedBotPctAt70 &&
        !sPlayerbotAIConfig.eraCappedBotPctAt80)
        LOG_INFO("playerbots", "[RandomBotLevelMgr] Era caps disabled: every bot levels to the cap.");
    else
        LOG_INFO("playerbots",
            "[RandomBotLevelMgr] Era caps: {}% of random bots stop at level 60, {}% at 70, {}% at 80. Bots below "
            "their ceiling are {}.",
            sPlayerbotAIConfig.eraCappedBotPctAt60, sPlayerbotAIConfig.eraCappedBotPctAt70,
            sPlayerbotAIConfig.eraCappedBotPctAt80,
            sPlayerbotAIConfig.eraCappedBotPromoteBelowCap ? "moved up onto it" : "left to level into it");

    if (sPlayerbotAIConfig.eraCappedBotGearQuality)
        LOG_INFO("playerbots",
                 "[RandomBotLevelMgr] Era gear: quality {} up to item level {} at 60, {} at 70, {} at 80.",
                 sPlayerbotAIConfig.eraCappedBotGearQuality, sPlayerbotAIConfig.eraCappedBotGearIlvlAt60,
                 sPlayerbotAIConfig.eraCappedBotGearIlvlAt70, sPlayerbotAIConfig.eraCappedBotGearIlvlAt80);

    if (!sPlayerbotAIConfig.resetBotLevelEnabled)
        LOG_INFO("playerbots", "[RandomBotLevelMgr] Level reset sub-feature disabled via configuration.");
    else
    {
        LOG_INFO("playerbots",
            "[RandomBotLevelMgr] Level reset loaded. MaxLevel = {} ({}), ResetToLevel = {}, SkipFromLevel = {} ({}), "
            "SkipToLevel = {}, ResetChance = {}%, ScaledChance = {}, RestrictTimePlayed = {}, "
            "IgnoreGuildBotsWithRealPlayers = {}, ExcludedNames = {}.",
            static_cast<int>(sPlayerbotAIConfig.resetBotLevelMaxLevel),
            sPlayerbotAIConfig.resetBotLevelMaxLevel > 0 ? "Enabled" : "Disabled",
            static_cast<int>(sPlayerbotAIConfig.resetBotLevelResetTo),
            static_cast<int>(sPlayerbotAIConfig.resetBotLevelSkipFrom),
            sPlayerbotAIConfig.resetBotLevelSkipFrom > 0 ? "Enabled" : "Disabled",
            static_cast<int>(sPlayerbotAIConfig.resetBotLevelSkipTo),
            static_cast<int>(sPlayerbotAIConfig.resetBotLevelChance),
            sPlayerbotAIConfig.resetBotLevelScaledChance ? "Enabled" : "Disabled",
            sPlayerbotAIConfig.resetBotLevelRestrictTimePlayed ? "Enabled" : "Disabled",
            sPlayerbotAIConfig.resetBotLevelIgnoreGuildWithRealPlayers ? "Enabled" : "Disabled",
            sPlayerbotAIConfig.resetBotLevelExcludeNames.empty()
                ? "None"
                : std::to_string(sPlayerbotAIConfig.resetBotLevelExcludeNames.size()) + " names");
    }
}

// Clamps bracket bounds to [_randomBotMinLevel, _randomBotMaxLevel] and rebalances the desired
// percentages (per faction) back to summing to 100, if they don't already.
void RandomBotLevelMgr::ClampAndBalanceBrackets()
{
    for (uint8 i = 0; i < _numRanges; ++i)
    {
        if (_allianceRanges[i].lower < _randomBotMinLevel)
            _allianceRanges[i].lower = _randomBotMinLevel;
        if (_allianceRanges[i].upper > _randomBotMaxLevel)
            _allianceRanges[i].upper = _randomBotMaxLevel;
        if (_allianceRanges[i].lower > _allianceRanges[i].upper)
            _allianceRanges[i].pct = 0;
    }
    for (uint8 i = 0; i < _numRanges; ++i)
    {
        if (_hordeRanges[i].lower < _randomBotMinLevel)
            _hordeRanges[i].lower = _randomBotMinLevel;
        if (_hordeRanges[i].upper > _randomBotMaxLevel)
            _hordeRanges[i].upper = _randomBotMaxLevel;
        if (_hordeRanges[i].lower > _hordeRanges[i].upper)
            _hordeRanges[i].pct = 0;
    }

    uint32 totalAlliance = 0;
    uint32 totalHorde = 0;
    for (uint8 i = 0; i < _numRanges; ++i)
    {
        totalAlliance += _allianceRanges[i].pct;
        totalHorde += _hordeRanges[i].pct;
    }

    if (totalAlliance != 100 && totalAlliance > 0)
    {
        LOG_TRACE("playerbots",
            "[RandomBotLevelMgr] Alliance: Sum of percentages is {} (expected 100). Auto adjusting.", totalAlliance);
        int missing = 100 - totalAlliance;
        while (missing > 0)
        {
            for (uint8 i = 0; i < _numRanges && missing > 0; ++i)
            {
                if (_allianceRanges[i].lower <= _allianceRanges[i].upper && _allianceRanges[i].pct > 0)
                {
                    _allianceRanges[i].pct++;
                    missing--;
                }
            }
        }
    }
    if (totalHorde != 100 && totalHorde > 0)
    {
        LOG_TRACE("playerbots", "[RandomBotLevelMgr] Horde: Sum of percentages is {} (expected 100). Auto adjusting.",
            totalHorde);
        int missing = 100 - totalHorde;
        while (missing > 0)
        {
            for (uint8 i = 0; i < _numRanges && missing > 0; ++i)
            {
                if (_hordeRanges[i].lower <= _hordeRanges[i].upper && _hordeRanges[i].pct > 0)
                {
                    _hordeRanges[i].pct++;
                    missing--;
                }
            }
        }
    }
}

// Normalizes real-player-census weights into desiredPercent values that sum to 100 for one
// faction's working bracket vector. Shared by both the synced and per-faction weighting branches.
void RandomBotLevelMgr::ApplyBracketWeights(std::vector<LevelBracketConfig>& ranges, std::vector<float> const& weights)
{
    float total = 0.0f;
    for (uint8 i = 0; i < _numRanges; ++i)
        total += weights[i];

    int pctSum = 0;
    for (uint8 i = 0; i < _numRanges; ++i)
    {
        uint8 pct = (total > 0.0f) ? static_cast<uint8>(std::round((weights[i] / total) * 100)) : 0;
        ranges[i].pct = pct;
        pctSum += pct;
    }
    // Fix rounding drift so sum = 100.
    int missing = 100 - pctSum;
    for (uint8 i = 0; i < _numRanges && missing > 0; ++i)
    {
        if (ranges[i].lower <= ranges[i].upper && ranges[i].pct > 0)
        {
            ranges[i].pct++;
            missing--;
        }
    }
}

// Returns the index of the level range containing the given level for the given team.
int RandomBotLevelMgr::GetLevelRangeIndex(uint8 level, TeamId team)
{
    if (level < _randomBotMinLevel || level > _randomBotMaxLevel)
        return -1;

    if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
        return -1;

    std::vector<LevelBracketConfig> const& ranges = GetFactionRanges(team);
    for (uint8 i = 0; i < _numRanges; ++i)
    {
        if (level >= ranges[i].lower && level <= ranges[i].upper)
            return i;
    }

    return -1;
}

// Adjusts a bot's level to fit within the given bracket for its faction. Death Knights are never
// assigned below CONFIG_START_HEROIC_PLAYER_LEVEL. The faction's level ranges are resolved via
// GetFactionRanges() at call time (rather than through a cached reference), since those vectors can
// be resized on a config reload.
void RandomBotLevelMgr::AdjustBotToRange(Player* bot, int targetRangeIndex, TeamId team)
{
    if (!bot || !bot->IsInWorld() || !bot->GetSession() || bot->GetSession()->isLogingOut() ||
        bot->IsDuringRemoveFromWorld())
        return;

    if (targetRangeIndex < 0 || targetRangeIndex >= _numRanges)
        return;

    std::vector<LevelBracketConfig> const& factionRanges = GetFactionRanges(team);
    if (static_cast<size_t>(targetRangeIndex) >= factionRanges.size())
        return;

    // A bot parked at an expansion ceiling is part of the old-content population and is not a
    // surplus body to be moved into whichever bracket is short. Redistribution is off by default,
    // but if an operator turns it on it must not quietly empty the level 60 and 70 tiers.
    if (uint8 const cap = EraCapFor(bot); cap && bot->GetLevel() == cap)
        return;

    if (bot->IsMounted())
        bot->Dismount();

    uint8 botOriginalLevel = bot->GetLevel();
    uint8 newLevel = 0;

    uint8 dkMinLevel = static_cast<uint8>(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));

    if (bot->getClass() == CLASS_DEATH_KNIGHT)
    {
        uint8 lowerBound = factionRanges[targetRangeIndex].lower;
        uint8 upperBound = factionRanges[targetRangeIndex].upper;
        if (upperBound < dkMinLevel)
        {
            LOG_TRACE("playerbots",
                "[RandomBotLevelMgr] AdjustBotToRange: Cannot assign {} Death Knight '{}' ({}) to range {}-{} "
                "(below level {}).",
                (team == TEAM_ALLIANCE) ? "Alliance" : "Horde", bot->GetName(), botOriginalLevel, lowerBound,
                upperBound, dkMinLevel);
            return;
        }
        if (lowerBound < dkMinLevel)
            lowerBound = dkMinLevel;
        if (lowerBound > upperBound)
            return;
        newLevel = urand(lowerBound, upperBound);
    }
    else
    {
        LevelBracketConfig const& range = factionRanges[targetRangeIndex];
        if (range.lower > range.upper)
        {
            LOG_TRACE("playerbots", "[RandomBotLevelMgr] AdjustBotToRange: Invalid range {}-{} for {} bot '{}'.",
                range.lower, range.upper, (team == TEAM_ALLIANCE) ? "Alliance" : "Horde", bot->GetName());
            return;
        }
        newLevel = urand(range.lower, range.upper);
    }

    PlayerbotFactory newFactory(bot, newLevel);
    newFactory.Randomize(false);

    // Force reset talents if equipment and spec persistence is enabled and the bot rolled to max
    // level. This works around an issue with how randomization interacts with equipment/spec
    // persistence for max-level bots.
    if (newLevel == _randomBotMaxLevel && sPlayerbotAIConfig.equipAndSpecPersistence)
    {
        PlayerbotFactory tempFactory(bot, newLevel);
        tempFactory.InitTalentsTree(false, true, true);
    }

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    LOG_TRACE("playerbots",
        "[RandomBotLevelMgr] AdjustBotToRange: {} Bot '{}' - {} ({}) adjusted to level {} (target range {}-{}).",
        (team == TEAM_ALLIANCE) ? "Alliance" : "Horde", bot->GetName(),
        botAI ? botAI->GetChatHelper()->FormatClass(bot->getClass()) : "Unknown", botOriginalLevel, newLevel,
        factionRanges[targetRangeIndex].lower, factionRanges[targetRangeIndex].upper);
}

// Loads the list of social friend low GUIDs (character_social, flags = 1) into _socialFriendsList.
void RandomBotLevelMgr::LoadSocialFriendList()
{
    _socialFriendsList.clear();
    QueryResult result = CharacterDatabase.Query("SELECT friend FROM character_social WHERE flags = 1");

    if (!result || result->GetRowCount() == 0)
        return;

    do
    {
        _socialFriendsList.push_back(result->Fetch()->Get<uint32>());
    } while (result->NextRow());
}

// Returns the bracket index for a player, flagging it for a pending level reset (to the closest
// bracket) if it currently falls outside every defined range for its faction.
//
// Only random bots are ever enqueued into _pendingLevelResets. This is also called for real
// players during the dynamic-distribution real-player census; a real player outside all brackets
// simply returns -1 (its census contribution is skipped) rather than being queued for a level
// reset - queuing a real player here would eventually reset that player's level, which is a bug
// inherited from the original module that this port fixes.
int RandomBotLevelMgr::GetOrFlagPlayerBracket(Player* player)
{
    bool isRandomBot = sRandomPlayerbotMgr.IsRandomBot(player);

    if (isRandomBot && IsNameInExcludeList(player, sPlayerbotAIConfig.levelBracketsExcludeNames))
        return -1;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (isRandomBot && sPlayerbotAIConfig.levelBracketsIgnoreGuildWithRealPlayers && botAI && botAI->IsInRealGuild())
        return -1;

    if (isRandomBot && sPlayerbotAIConfig.levelBracketsIgnoreArenaTeamBots && BotInArenaTeam(player))
        return -1;

    // Exclude bots grouped with a real player from bracket processing.
    if (isRandomBot)
    {
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->IsInWorld() && !GET_PLAYERBOT_AI(member))
                    return -1;
            }
        }
    }

    TeamId team = player->GetTeamId();
    int rangeIndex = GetLevelRangeIndex(player->GetLevel(), team);
    if (rangeIndex >= 0)
        return rangeIndex;

    if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
        return -1;

    // Only random bots may be queued for a level reset below. Real players (and non-random bots)
    // outside every bracket simply fall through and return -1.
    if (!isRandomBot)
        return -1;

    std::vector<LevelBracketConfig> const& factionRanges = GetFactionRanges(team);

    int targetRange = -1;
    int smallestDiff = std::numeric_limits<int>::max();
    uint8 dkMinLevel = static_cast<uint8>(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
    for (int i = 0; i < _numRanges; ++i)
    {
        if (factionRanges[i].lower > factionRanges[i].upper)
            continue;

        // Skip brackets that Death Knights cannot be assigned to.
        if (player->getClass() == CLASS_DEATH_KNIGHT && factionRanges[i].upper < dkMinLevel)
            continue;

        int diff = 0;
        if (player->GetLevel() < factionRanges[i].lower)
            diff = factionRanges[i].lower - player->GetLevel();
        else if (player->GetLevel() > factionRanges[i].upper)
            diff = player->GetLevel() - factionRanges[i].upper;
        if (diff < smallestDiff)
        {
            smallestDiff = diff;
            targetRange = i;
        }
    }

    if (targetRange >= 0)
    {
        bool alreadyFlagged = false;
        ObjectGuid guid = player->GetGUID();
        for (auto const& entry : _pendingLevelResets)
        {
            if (entry.botGuid == guid)
            {
                alreadyFlagged = true;
                break;
            }
        }
        if (!alreadyFlagged)
            _pendingLevelResets.push_back({guid, targetRange, team});
    }

    return -1;
}

// Moves bots from an over-populated range into ranges that still need bots. Called once for
// safeBots and once for flaggedBots by ProcessFactionDistribution.
void RandomBotLevelMgr::RedistributeSurplusBots(std::vector<Player*>& sourceBots, int fromRange, TeamId team,
    std::vector<int>& actualCounts, std::vector<int> const& desiredCounts, std::vector<int> const& targetRanges)
{
    size_t targetIdx = 0;
    while (actualCounts[fromRange] > desiredCounts[fromRange] && !sourceBots.empty() && targetIdx < targetRanges.size())
    {
        Player* bot = sourceBots.back();
        sourceBots.pop_back();

        int targetRange = targetRanges[targetIdx];
        if (actualCounts[targetRange] >= desiredCounts[targetRange])
        {
            ++targetIdx;
            continue;
        }

        ObjectGuid botGuid = bot->GetGUID();
        bool alreadyFlagged = false;
        for (auto const& entry : _pendingLevelResets)
        {
            if (entry.botGuid == botGuid)
            {
                alreadyFlagged = true;
                break;
            }
        }
        if (!alreadyFlagged)
            _pendingLevelResets.push_back({botGuid, targetRange, team});

        actualCounts[fromRange]--;
        actualCounts[targetRange]++;
        if (actualCounts[targetRange] >= desiredCounts[targetRange])
            ++targetIdx;
    }
}

// Computes desired-vs-actual counts for one faction and flags surplus bots (safe bots first, then
// flagged ones) for a pending reset into under-populated ranges. Shared by both factions to avoid
// duplicating the ~100-line Alliance/Horde block that used to exist here.
void RandomBotLevelMgr::ProcessFactionDistribution(TeamId team, uint32 totalBots, std::vector<int>& actualCounts,
    std::vector<std::vector<Player*>>& botsByRange)
{
    if (totalBots == 0)
        return;

    std::vector<LevelBracketConfig> const& ranges = GetFactionRanges(team);
    char const* factionName = (team == TEAM_ALLIANCE) ? "Alliance" : "Horde";

    std::vector<int> desiredCounts(_numRanges, 0);
    for (uint8 i = 0; i < _numRanges; ++i)
    {
        desiredCounts[i] = static_cast<int>(std::round((ranges[i].pct / 100.0) * totalBots));
        LOG_DEBUG("playerbots", "[RandomBotLevelMgr] {} Range {} ({}-{}): Desired = {}, Actual = {}.",
            factionName, i + 1, ranges[i].lower, ranges[i].upper, desiredCounts[i], actualCounts[i]);
    }

    for (uint8 i = 0; i < _numRanges; ++i)
    {
        std::vector<Player*> safeBots;
        std::vector<Player*> flaggedBots;
        for (Player* bot : botsByRange[i])
        {
            if (IsBotSafeForLevelReset(bot))
                safeBots.push_back(bot);
            else
                flaggedBots.push_back(bot);
        }

        std::vector<int> targetRanges;
        for (uint8 j = 0; j < _numRanges; ++j)
        {
            if (actualCounts[j] < desiredCounts[j])
                targetRanges.push_back(j);
        }

        RedistributeSurplusBots(safeBots, i, team, actualCounts, desiredCounts, targetRanges);
        RedistributeSurplusBots(flaggedBots, i, team, actualCounts, desiredCounts, targetRanges);
    }
}

// Runs the periodic bot level distribution pass: optionally recalculates dynamic bracket
// percentages based on the real-player census, then flags surplus bots in over-populated brackets
// for a pending reset into under-populated ones (per faction, via ProcessFactionDistribution).
void RandomBotLevelMgr::RunLevelBracketsDistribution()
{
    auto const& allPlayers = ObjectAccessor::GetPlayers();

    LoadSocialFriendList();

    if (sPlayerbotAIConfig.levelBracketsDynamicDistribution)
    {
        // Calculate real player bracket counts.
        std::vector<int> allianceRealCounts(_numRanges, 0);
        std::vector<int> hordeRealCounts(_numRanges, 0);
        uint32 totalAllianceReal = 0;
        uint32 totalHordeReal = 0;

        for (auto const& itr : allPlayers)
        {
            Player* player = itr.second;
            if (!player || !player->IsInWorld())
                continue;
            if (GET_PLAYERBOT_AI(player))
                continue; // Only count real players.
            int rangeIndex = GetOrFlagPlayerBracket(player);
            if (rangeIndex < 0)
                continue;
            if (player->GetTeamId() == TEAM_ALLIANCE)
            {
                allianceRealCounts[rangeIndex]++;
                totalAllianceReal++;
            }
            else if (player->GetTeamId() == TEAM_HORDE)
            {
                hordeRealCounts[rangeIndex]++;
                totalHordeReal++;
            }
        }

        float const baseline = 1.0f;
        std::vector<float> allianceWeights(_numRanges, 0.0f);
        std::vector<float> hordeWeights(_numRanges, 0.0f);

        // SYNCED MODE: real player weighting is combined for both factions, applied to both bracket tables.
        if (sPlayerbotAIConfig.levelBracketsSyncFactions)
        {
            uint32 totalCombinedReal = totalAllianceReal + totalHordeReal;
            for (uint8 i = 0; i < _numRanges; ++i)
            {
                int combinedReal = allianceRealCounts[i] + hordeRealCounts[i];
                float weight = baseline + sPlayerbotAIConfig.levelBracketsRealPlayerWeight *
                    (totalCombinedReal > 0 ? (1.0f / float(totalCombinedReal)) : 1.0f) * std::log(1 + combinedReal);
                allianceWeights[i] = weight;
                hordeWeights[i] = weight;
            }
        }
        else
        {
            // Separate dynamic weighting for each faction.
            for (uint8 i = 0; i < _numRanges; ++i)
            {
                if (_allianceRanges[i].lower > _allianceRanges[i].upper)
                    allianceWeights[i] = 0.0f;
                else
                    allianceWeights[i] = baseline + sPlayerbotAIConfig.levelBracketsRealPlayerWeight *
                        (totalAllianceReal > 0 ? (1.0f / totalAllianceReal) : 1.0f) *
                        std::log(1 + allianceRealCounts[i]);

                if (_hordeRanges[i].lower > _hordeRanges[i].upper)
                    hordeWeights[i] = 0.0f;
                else
                    hordeWeights[i] = baseline + sPlayerbotAIConfig.levelBracketsRealPlayerWeight *
                        (totalHordeReal > 0 ? (1.0f / totalHordeReal) : 1.0f) * std::log(1 + hordeRealCounts[i]);
            }
        }

        ApplyBracketWeights(_allianceRanges, allianceWeights);
        ApplyBracketWeights(_hordeRanges, hordeWeights);

        // Ensure brackets respect global min/max levels and percentages sum to 100.
        ClampAndBalanceBrackets();

        for (uint8 i = 0; i < _numRanges; ++i)
            LOG_DEBUG("playerbots",
                "[RandomBotLevelMgr] Final Range {}: {}-{}, Alliance Desired: {}%, Horde Desired: {}%", i + 1,
                _allianceRanges[i].lower, _allianceRanges[i].upper, _allianceRanges[i].pct, _hordeRanges[i].pct);
    }

    uint32 totalAllianceBots = 0;
    std::vector<int> allianceActualCounts(_numRanges, 0);
    std::vector<std::vector<Player*>> allianceBotsByRange(_numRanges);

    uint32 totalHordeBots = 0;
    std::vector<int> hordeActualCounts(_numRanges, 0);
    std::vector<std::vector<Player*>> hordeBotsByRange(_numRanges);

    for (auto const& itr : allPlayers)
    {
        Player* player = itr.second;
        if (!player || !player->IsInWorld())
            continue;
        if (!sRandomPlayerbotMgr.IsRandomBot(player))
            continue;
        if (IsNameInExcludeList(player, sPlayerbotAIConfig.levelBracketsExcludeNames))
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (sPlayerbotAIConfig.levelBracketsIgnoreGuildWithRealPlayers && botAI && botAI->IsInRealGuild())
            continue;
        if (sPlayerbotAIConfig.levelBracketsIgnoreFriendListed && BotInFriendList(player, _socialFriendsList))
            continue;
        if (sPlayerbotAIConfig.levelBracketsIgnoreArenaTeamBots && BotInArenaTeam(player))
            continue;

        if (player->GetTeamId() == TEAM_ALLIANCE)
        {
            totalAllianceBots++;
            int rangeIndex = GetOrFlagPlayerBracket(player);
            if (rangeIndex >= 0)
            {
                allianceActualCounts[rangeIndex]++;
                allianceBotsByRange[rangeIndex].push_back(player);
            }
        }
        else if (player->GetTeamId() == TEAM_HORDE)
        {
            totalHordeBots++;
            int rangeIndex = GetOrFlagPlayerBracket(player);
            if (rangeIndex >= 0)
            {
                hordeActualCounts[rangeIndex]++;
                hordeBotsByRange[rangeIndex].push_back(player);
            }
        }
    }

    LOG_DEBUG("playerbots", "[RandomBotLevelMgr] Total Alliance Bots: {}. Total Horde Bots: {}.",
        totalAllianceBots, totalHordeBots);

    ProcessFactionDistribution(TEAM_ALLIANCE, totalAllianceBots, allianceActualCounts, allianceBotsByRange);
    ProcessFactionDistribution(TEAM_HORDE, totalHordeBots, hordeActualCounts, hordeBotsByRange);

    LOG_DEBUG("playerbots",
        "[RandomBotLevelMgr] Distribution adjustment complete. Alliance bots: {}, Horde bots: {}.", totalAllianceBots,
        totalHordeBots);
}

// Processes the pending level reset queue, applying up to FlaggedProcessLimit resets per cycle
// (0 = unlimited). Bots are dropped from the queue if they've gone offline, become excluded, joined
// a guild/friend-list/arena-team/group that should protect them, stopped being a random bot, or
// otherwise stopped being eligible; they are reset (and dropped) once they're confirmed safe.
void RandomBotLevelMgr::ProcessPendingLevelResets()
{
    if (_pendingLevelResets.empty())
        return;

    uint32 processed = 0;
    for (auto it = _pendingLevelResets.begin(); it != _pendingLevelResets.end();)
    {
        if (sPlayerbotAIConfig.levelBracketsFlaggedProcessLimit > 0 &&
            processed >= sPlayerbotAIConfig.levelBracketsFlaggedProcessLimit)
            break;

        Player* bot = ObjectAccessor::FindPlayer(it->botGuid);

        if (!bot || !bot->IsInWorld() || !bot->GetSession() || bot->GetSession()->isLogingOut() ||
            bot->IsDuringRemoveFromWorld())
        {
            it = _pendingLevelResets.erase(it);
            continue;
        }

        // Defensive: never resolve a queue entry to anything but a random bot. Real players must
        // never end up in this queue (see GetOrFlagPlayerBracket), but guard here too in case a
        // player's bot status changes between enqueue and processing.
        if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            it = _pendingLevelResets.erase(it);
            continue;
        }

        if (IsNameInExcludeList(bot, sPlayerbotAIConfig.levelBracketsExcludeNames))
        {
            it = _pendingLevelResets.erase(it);
            continue;
        }

        int targetRange = it->targetRange;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (sPlayerbotAIConfig.levelBracketsIgnoreGuildWithRealPlayers && botAI && botAI->IsInRealGuild())
        {
            it = _pendingLevelResets.erase(it);
            continue;
        }

        if (sPlayerbotAIConfig.levelBracketsIgnoreFriendListed && BotInFriendList(bot, _socialFriendsList))
        {
            it = _pendingLevelResets.erase(it);
            continue;
        }

        if (sPlayerbotAIConfig.levelBracketsIgnoreArenaTeamBots && BotInArenaTeam(bot))
        {
            it = _pendingLevelResets.erase(it);
            continue;
        }

        // Check if the bot is now grouped with a real player.
        if (Group* group = bot->GetGroup())
        {
            bool hasRealPlayer = false;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->IsInWorld() && !GET_PLAYERBOT_AI(member))
                {
                    hasRealPlayer = true;
                    break;
                }
            }
            if (hasRealPlayer)
            {
                it = _pendingLevelResets.erase(it);
                continue;
            }
        }

        if (IsBotSafeForLevelReset(bot))
        {
            AdjustBotToRange(bot, targetRange, it->team);
            it = _pendingLevelResets.erase(it);
            ++processed;
        }
        else
            ++it;
    }
}

// =============================================================================
// LEVEL RESET FEATURE
// =============================================================================

// Computes the percent chance that a bot at the given level should be reset. When
// AiPlayerbot.ResetBotLevel.ScaledChance is enabled, the chance scales linearly from 0 at level 1
// up to AiPlayerbot.ResetBotLevel.ResetChance at AiPlayerbot.ResetBotLevel.MaxLevel.
uint8 RandomBotLevelMgr::ComputeResetChance(uint8 level) const
{
    uint8 chance = sPlayerbotAIConfig.resetBotLevelChance;
    if (sPlayerbotAIConfig.resetBotLevelScaledChance)
    {
        chance = static_cast<uint8>((static_cast<float>(level) / sPlayerbotAIConfig.resetBotLevelMaxLevel) *
            sPlayerbotAIConfig.resetBotLevelChance);
        LOG_DEBUG("playerbots",
            "[RandomBotLevelMgr] ComputeResetChance: For level {} / {} with scaling, computed chance = {}%", level,
            sPlayerbotAIConfig.resetBotLevelMaxLevel, chance);
    }
    else
        LOG_DEBUG("playerbots",
            "[RandomBotLevelMgr] ComputeResetChance: For level {} / {} without scaling, chance = {}%", level,
            sPlayerbotAIConfig.resetBotLevelMaxLevel, chance);
    return chance;
}

// Resets a bot down to AiPlayerbot.ResetBotLevel.ResetToLevel (or the Death Knight starting level,
// whichever is higher) via a full PlayerbotFactory randomize.
void RandomBotLevelMgr::ResetBot(Player* player, uint8 currentLevel)
{
    uint8 levelToResetTo = sPlayerbotAIConfig.resetBotLevelResetTo;

    uint8 dkMinLevel = static_cast<uint8>(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
    if (player->getClass() == CLASS_DEATH_KNIGHT && levelToResetTo < dkMinLevel)
        levelToResetTo = dkMinLevel;

    // Dismount before randomization to prevent a wrong mount at the new level.
    if (player->IsMounted())
        player->Dismount();

    PlayerbotFactory newFactory(player, levelToResetTo);
    newFactory.Randomize(false);

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    LOG_DEBUG("playerbots", "[RandomBotLevelMgr] ResetBot: Bot '{}' - {} at level {} was reset to level {}.",
        player->GetName(), botAI ? botAI->GetChatHelper()->FormatClass(player->getClass()) : "Unknown", currentLevel,
        levelToResetTo);
}

// Sends a bot straight to AiPlayerbot.ResetBotLevel.SkipToLevel (or the Death Knight starting
// level, whichever is higher) via a full PlayerbotFactory randomize.
void RandomBotLevelMgr::SkipBotLevel(Player* player, uint8 currentLevel)
{
    uint8 levelToSkipTo = sPlayerbotAIConfig.resetBotLevelSkipTo;

    uint8 dkMinLevel = static_cast<uint8>(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
    if (player->getClass() == CLASS_DEATH_KNIGHT && levelToSkipTo < dkMinLevel)
        levelToSkipTo = dkMinLevel;

    // Dismount before randomization to prevent a wrong mount at the new level.
    if (player->IsMounted())
        player->Dismount();

    PlayerbotFactory newFactory(player, levelToSkipTo);
    newFactory.Randomize(false);

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    LOG_DEBUG("playerbots", "[RandomBotLevelMgr] SkipBotLevel: Bot '{}' - {} at level {} was skipped to level {}.",
        player->GetName(), botAI ? botAI->GetChatHelper()->FormatClass(player->getClass()) : "Unknown", currentLevel,
        levelToSkipTo);
}

// Runs the periodic played-time-based reset check for bots sitting at or above MaxLevel. Only
// reached when Enabled, RestrictTimePlayed, and MaxLevel > 0 (see Update()).
void RandomBotLevelMgr::RunResetPlayedTimeCheck()
{
    LOG_DEBUG("playerbots", "[RandomBotLevelMgr] OnUpdate: Starting time-based reset check...");

    auto const& allPlayers = ObjectAccessor::GetPlayers();
    for (auto const& itr : allPlayers)
    {
        Player* candidate = itr.second;
        if (!candidate || !candidate->IsInWorld())
            continue;
        if (!sRandomPlayerbotMgr.IsRandomBot(candidate))
            continue;

        if (IsNameInExcludeList(candidate, sPlayerbotAIConfig.resetBotLevelExcludeNames))
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(candidate);
        if (sPlayerbotAIConfig.resetBotLevelIgnoreGuildWithRealPlayers && botAI && botAI->IsInRealGuild())
            continue;

        uint8 currentLevel = candidate->GetLevel();
        if (currentLevel < sPlayerbotAIConfig.resetBotLevelMaxLevel)
            continue;

        // A bot parked on its era ceiling stays there.
        //
        // This check recycles bots that have sat at the level cap long enough, which is how the roster
        // keeps a spread of levels instead of collecting at the top. A bot assigned to an era is the
        // opposite case: sitting at its ceiling is its whole purpose, and it is the population a raid
        // is filled from. A ceiling below the level cap was already safe here by accident -- the test
        // above skips it -- but the level 80 era sits exactly on the cap, so without this the 80
        // population would be recycled away as fast as it was built.
        if (uint8 const cap = EraCapFor(candidate); cap && currentLevel == cap)
            continue;

        // Only reset if the bot has played at least MinTimePlayed seconds at this level.
        if (candidate->GetLevelPlayedTime() < sPlayerbotAIConfig.resetBotLevelMinTimePlayed)
        {
            LOG_DEBUG("playerbots",
                "[RandomBotLevelMgr] OnUpdate: Bot '{}' at level {} has insufficient played time ({} < {} "
                "seconds).",
                candidate->GetName(), currentLevel, candidate->GetLevelPlayedTime(),
                sPlayerbotAIConfig.resetBotLevelMinTimePlayed);
            continue;
        }

        uint8 resetChance = ComputeResetChance(currentLevel);
        LOG_DEBUG("playerbots",
            "[RandomBotLevelMgr] OnUpdate: Bot '{}' qualifies for time-based reset. Level: {}, "
            "LevelPlayedTime: {} seconds, computed reset chance: {}%.",
            candidate->GetName(), currentLevel, candidate->GetLevelPlayedTime(), resetChance);
        if (urand(0, 99) < resetChance)
        {
            LOG_DEBUG("playerbots",
                "[RandomBotLevelMgr] OnUpdate: Reset chance check passed for bot '{}'. Resetting bot.",
                candidate->GetName());
            ResetBot(candidate, currentLevel);
        }
    }
}

// =============================================================================
// SHARED UPDATE / HOOKS
// =============================================================================

/**
 * Bring bots that are above their era ceiling down onto it.
 *
 * Without this the feature is correct and invisible. Levels are handed out by RandomizeFirst, which
 * a bot reaches somewhere between MinRandomBotRandomizeTime and MaxRandomBotRandomizeTime -- two
 * hours and *fourteen days* on this realm. So the clamp only bites when a bot happens to be
 * re-rolled, and the bots already above their ceiling when the feature was switched on would take
 * weeks to come back round. Measured at the moment of enabling: 50 characters above their ceiling,
 * exactly one parked on it. Old-era goods would not reach the auction house this month.
 *
 * Deliberately bounded and gradual rather than a sweep at startup: a batch that re-gears fifty bots
 * in one tick is a stall, and this has to share the world thread with everything else. It is also
 * self-limiting -- once the clamps in RandomizeFirst and IncreaseLevel are in place nothing climbs
 * above its ceiling again, so after the initial convergence this pass finds nothing and costs a
 * loop over the online players.
 *
 * The bots it moves are random bots, which this module already recycles to level 1 outright when
 * ResetBotLevel fires. Re-rolling one at 60 instead of 75 is a smaller change than that, and it
 * re-gears them through the normal factory path, which is what makes them era-appropriate rather
 * than merely era-levelled. A person's own characters are never touched: ApplyXpGainPolicy and this
 * both require IsRandomBot.
 */
void RandomBotLevelMgr::RunEraSeedingPass()
{
    uint32 const limit = sPlayerbotAIConfig.eraCappedBotSeedPerPass;
    if (!limit)
        return;

    uint32 moved = 0;

    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        if (moved >= limit)
            break;

        Player* bot = itr.second;
        if (!bot || !bot->IsInWorld() || !bot->GetSession() || bot->GetSession()->isLogingOut() ||
            bot->IsDuringRemoveFromWorld())
            continue;

        if (!sRandomPlayerbotMgr.IsRandomBot(bot))
            continue;

        uint8 const cap = EraCapFor(bot);
        if (!cap || bot->GetLevel() == cap)
            continue;

        // Below its ceiling, not above it.
        //
        // This pass was written to bring bots down onto a ceiling they had already climbed past, which
        // is the only direction that matters on a realm whose bots are already at level. It is the
        // wrong direction for the population an operator actually wants: a bot assigned to the level
        // 60 era and currently level 12 has to earn forty-eight levels before it joins that
        // population, and at ordinary XP rates that is months. Measured on this realm with 15% of the
        // roster assigned to each of 60 and 70: 83 characters in the whole 60-69 bracket and 533 still
        // below level 20, so a forty-man raid could not be filled from bots that belong to that era.
        //
        // Promotion is the same operation as the demotion below and as a bot's first roll: a level is
        // assigned and the factory re-gears to match. Nothing here is earned, in either direction.
        if (bot->GetLevel() < cap && !sPlayerbotAIConfig.eraCappedBotPromoteBelowCap)
            continue;

        // A death knight cannot exist below its starting level, so one assigned a ceiling beneath
        // that is simply not part of the old-content population.
        if (cap < sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL) && bot->getClass() == CLASS_DEATH_KNIGHT)
            continue;

        if (bot->IsInCombat() || bot->isDead() || bot->InBattleground() || bot->GetMap()->Instanceable())
            continue;

        LOG_INFO("playerbots", "[Era] {} is level {} but belongs to the level {} population, {} it there",
                 bot->GetName(), bot->GetLevel(), cap, bot->GetLevel() < cap ? "bringing" : "re-rolling");

        uint32 quality = 0;
        uint32 ilvl = 0;
        EraGearTarget(cap, quality, ilvl);

        // Dressed for the era, not to the realm's default quality. Passing a quality explicitly is what
        // lets the factory consider epics at all: it matches quality exactly and only ever walks down from
        // what it is given, so the default of rare is a ceiling and not a starting point.
        PlayerbotFactory factory(bot, cap, quality, PlayerbotFactory::CalcMixedGearScore(ilvl, quality));
        factory.Randomize(false);
        ApplyXpGainPolicy(bot);

        // Finish the job the way an ordinary re-roll does.
        //
        // RandomizeFirst does three more things after the factory: it resets the AI, drops the bot's
        // group, and teleports it somewhere suited to its new level. Re-gearing without them leaves a
        // bot with a new level standing exactly where the old one was, and for a bot brought *down*
        // onto its ceiling that is lethal -- a level 80 in Icecrown re-rolled to 60 dies on the spot,
        // repeatedly, and it was doing so here in numbers once promotion made re-rolls common: sixty-
        // five bots releasing their spirits over and over in five minutes. Upwards it is merely wrong:
        // a freshly minted level 80 standing in a starter zone.
        //
        // The group has to go too. Its other members were chosen for the level this bot used to be.
        if (PlayerbotAI* rerolled = GET_PLAYERBOT_AI(bot))
        {
            rerolled->Reset(true);

            if (bot->GetGroup())
                rerolled->LeaveOrDisbandGroup();
        }

        sRandomPlayerbotMgr.RandomTeleportForLevel(bot);

        ++moved;
        ++_eraSeeded;
    }
}

void RandomBotLevelMgr::EraGearTarget(uint8 cap, uint32& quality, uint32& ilvl)
{
    quality = sPlayerbotAIConfig.eraCappedBotGearQuality;

    switch (cap)
    {
        case 60:
            ilvl = sPlayerbotAIConfig.eraCappedBotGearIlvlAt60;
            break;
        case 70:
            ilvl = sPlayerbotAIConfig.eraCappedBotGearIlvlAt70;
            break;
        default:
            ilvl = sPlayerbotAIConfig.eraCappedBotGearIlvlAt80;
            break;
    }
}

/**
 * Dress the bots that are already standing on their ceiling.
 *
 * The seeding pass only looks at bots whose *level* is wrong, so a bot that arrived at its ceiling
 * before this existed -- or was promoted there by an earlier build -- keeps the gear it had. That gear
 * is rare at best: the ordinary path gives every random bot RandomGearQualityLimit and InitEquipment
 * demands an exact quality match, so a random bot has never worn an epic. Twenty-five of them walked
 * into Magtheridon's Lair and all twenty-five died in three minutes.
 *
 * Judged on the same number the factory is given, an average mixed gear score per slot, so "geared for
 * this era" means the same thing in both directions. Incremental, so a bot that already has something
 * better in a slot keeps it, and bounded per pass because each one is a full re-gear on the world thread.
 */
void RandomBotLevelMgr::RunEraRegearPass()
{
    uint32 const limit = sPlayerbotAIConfig.eraCappedBotRegearPerPass;
    if (!limit || !sPlayerbotAIConfig.eraCappedBotGearQuality)
        return;

    uint32 regeared = 0;

    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        if (regeared >= limit)
            break;

        Player* bot = itr.second;
        if (!bot || !bot->IsInWorld() || !bot->GetSession() || bot->GetSession()->isLogingOut() ||
            bot->IsDuringRemoveFromWorld())
            continue;

        if (!sRandomPlayerbotMgr.IsRandomBot(bot))
            continue;

        uint8 const cap = EraCapFor(bot);
        if (!cap || bot->GetLevel() != cap)
            continue;

        // Never mid-content. A re-gear strips and replaces what the bot is wearing, which is not something
        // to do to a raider halfway up Icecrown, and the raid it is in would lose the fight and the gear.
        if (bot->IsInCombat() || bot->isDead() || bot->InBattleground() || bot->GetGroup())
            continue;

        if (Map* map = bot->FindMap(); map && map->Instanceable())
            continue;

        uint32 quality = 0;
        uint32 ilvl = 0;
        EraGearTarget(cap, quality, ilvl);

        uint32 const target = PlayerbotFactory::CalcMixedGearScore(ilvl, quality);
        uint32 const current = PlayerbotAI::GetMixedGearScore(bot, false, false, 0);

        if (!target || current >= uint32(float(target) * sPlayerbotAIConfig.eraCappedBotRegearThreshold))
            continue;

        LOG_INFO("playerbots", "[Era] {} is level {} in gear worth {} and its era expects about {}; re-gearing",
                 bot->GetName(), cap, current, target);

        PlayerbotFactory::AutoGear(bot, quality, ilvl, true);

        ++regeared;
        ++_eraRegeared;
    }
}

void RandomBotLevelMgr::Update(uint32 diff)
{
    if (sPlayerbotAIConfig.levelBracketsEnabled)
    {
        _bracketsTimer += diff;
        _flaggedTimer += diff;

        if (_flaggedTimer >= sPlayerbotAIConfig.levelBracketsFlaggedCheckFrequency * 1000)
        {
            ProcessPendingLevelResets();
            _flaggedTimer = 0;
        }

        if (_bracketsTimer >= sPlayerbotAIConfig.levelBracketsCheckFrequency * 1000)
        {
            _bracketsTimer = 0;
            RunLevelBracketsDistribution();
        }
    }

    if (sPlayerbotAIConfig.eraCappedBotPctAt60 || sPlayerbotAIConfig.eraCappedBotPctAt70 ||
        sPlayerbotAIConfig.eraCappedBotPctAt80)
    {
        _eraSeedTimer += diff;
        if (_eraSeedTimer >= sPlayerbotAIConfig.eraCappedBotSeedIntervalMs)
        {
            _eraSeedTimer = 0;
            RunEraSeedingPass();
            RunEraRegearPass();
        }
    }

    if (sPlayerbotAIConfig.resetBotLevelEnabled && sPlayerbotAIConfig.resetBotLevelRestrictTimePlayed &&
        sPlayerbotAIConfig.resetBotLevelMaxLevel > 0)
    {
        _resetTimer += diff;
        if (_resetTimer >= sPlayerbotAIConfig.resetBotLevelPlayedTimeCheckFrequency * 1000)
        {
            _resetTimer = 0;
            RunResetPlayedTimeCheck();
        }
    }
}

void RandomBotLevelMgr::OnBotLogin(Player* player)
{
    if (!sRandomPlayerbotMgr.IsRandomBot(player))
        return;

    if (IsNameInExcludeList(player, sPlayerbotAIConfig.resetBotLevelExcludeNames))
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (sPlayerbotAIConfig.resetBotLevelIgnoreGuildWithRealPlayers && botAI && botAI->IsInRealGuild())
        return;

    uint8 currentLevel = player->GetLevel();

    if (sPlayerbotAIConfig.resetBotLevelMaxLevel > 0)
    {
        // Bot is above MaxLevel: reset immediately.
        if (currentLevel > sPlayerbotAIConfig.resetBotLevelMaxLevel)
        {
            LOG_DEBUG("playerbots",
                "[RandomBotLevelMgr] OnPlayerLogin: Bot '{}' above max level {}. Resetting immediately.",
                player->GetName(), sPlayerbotAIConfig.resetBotLevelMaxLevel);
            ResetBot(player, currentLevel);
            return;
        }

        // Bot is exactly at MaxLevel: apply the time-played restriction (if any) and chance.
        if (currentLevel == sPlayerbotAIConfig.resetBotLevelMaxLevel)
        {
            if (!sPlayerbotAIConfig.resetBotLevelRestrictTimePlayed ||
                player->GetLevelPlayedTime() >= sPlayerbotAIConfig.resetBotLevelMinTimePlayed)
            {
                uint8 resetChance = ComputeResetChance(currentLevel);
                if (urand(0, 99) < resetChance)
                {
                    LOG_DEBUG("playerbots",
                        "[RandomBotLevelMgr] OnPlayerLogin: Bot '{}' meets reset criteria. Resetting.",
                        player->GetName());
                    ResetBot(player, currentLevel);
                }
            }
        }
    }

    if (sPlayerbotAIConfig.resetBotLevelSkipFrom > 0 && currentLevel == sPlayerbotAIConfig.resetBotLevelSkipFrom)
    {
        LOG_DEBUG("playerbots",
            "[RandomBotLevelMgr] OnPlayerLogin: Bot '{}' at skip level {}. Applying skip.", player->GetName(),
            currentLevel);
        SkipBotLevel(player, currentLevel);
    }
}

void RandomBotLevelMgr::OnBotLevelChanged(Player* player, uint8 oldLevel)
{
    if (!sRandomPlayerbotMgr.IsRandomBot(player))
        return;

    // Only react to a natural level up.
    if (player->GetLevel() != oldLevel + 1)
        return;

    if (IsNameInExcludeList(player, sPlayerbotAIConfig.resetBotLevelExcludeNames))
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (sPlayerbotAIConfig.resetBotLevelIgnoreGuildWithRealPlayers && botAI && botAI->IsInRealGuild())
        return;

    uint8 newLevel = player->GetLevel();

    // The ceiling is checked before the skip and reset rules, because a bot that has just arrived
    // at its era's last level should not then be skipped past it or recycled out of it.
    if (uint8 const cap = EraCapFor(player); cap && newLevel == cap)
    {
        ApplyXpGainPolicy(player);
        return;
    }

    // SkipFromLevel takes priority and is not affected by ScaledChance or RestrictTimePlayed.
    if (sPlayerbotAIConfig.resetBotLevelSkipFrom > 0 && newLevel == sPlayerbotAIConfig.resetBotLevelSkipFrom)
    {
        LOG_DEBUG("playerbots",
            "[RandomBotLevelMgr] OnPlayerLevelChanged: Bot '{}' reached skip level {}. Skipping to level {}.",
            player->GetName(), newLevel, sPlayerbotAIConfig.resetBotLevelSkipTo);
        SkipBotLevel(player, newLevel);
        return;
    }

    if (sPlayerbotAIConfig.resetBotLevelMaxLevel == 0)
        return;

    // Strictly above MaxLevel: reset immediately regardless of time played.
    if (newLevel > sPlayerbotAIConfig.resetBotLevelMaxLevel)
    {
        LOG_DEBUG("playerbots",
            "[RandomBotLevelMgr] OnPlayerLevelChanged: Bot '{}' exceeded max level {}. Resetting immediately.",
            player->GetName(), sPlayerbotAIConfig.resetBotLevelMaxLevel);
        ResetBot(player, newLevel);
        return;
    }

    // Exactly at MaxLevel with a time-played restriction: defer to the OnUpdate timer.
    if (sPlayerbotAIConfig.resetBotLevelRestrictTimePlayed && newLevel == sPlayerbotAIConfig.resetBotLevelMaxLevel)
    {
        LOG_DEBUG("playerbots",
            "[RandomBotLevelMgr] OnPlayerLevelChanged: Bot '{}' at level {} deferred to OnUpdate due to "
            "time-played restriction.",
            player->GetName(), newLevel);
        return;
    }

    uint8 resetChance = ComputeResetChance(newLevel);
    if (sPlayerbotAIConfig.resetBotLevelScaledChance || newLevel >= sPlayerbotAIConfig.resetBotLevelMaxLevel)
    {
        LOG_DEBUG("playerbots",
            "[RandomBotLevelMgr] OnPlayerLevelChanged: Bot '{}' at level {} has reset chance {}%.",
            player->GetName(), newLevel, resetChance);
        if (urand(0, 99) < resetChance)
            ResetBot(player, newLevel);
    }
}

void RandomBotLevelMgr::OnPlayerLogout(Player* player)
{
    // Level brackets: drop the bot from the pending-reset queue. Safe to run even when the
    // brackets sub-feature is disabled, since the queue is then always empty.
    ObjectGuid guid = player->GetGUID();
    _pendingLevelResets.erase(
        std::remove_if(_pendingLevelResets.begin(), _pendingLevelResets.end(),
            [guid](PendingResetEntry const& entry) { return entry.botGuid == guid; }),
        _pendingLevelResets.end());
}

// =============================================================================
// WORLD SCRIPT
// =============================================================================
class RandomBotLevelWorldScript : public WorldScript
{
public:
    RandomBotLevelWorldScript()
        : WorldScript("RandomBotLevelWorldScript",
              { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_AFTER_CONFIG_LOAD })
    {
    }

    void OnStartup() override
    {
        RandomBotLevelMgr::instance().LoadConfig();
        RandomBotLevelMgr::instance().LogStartupSummary();
    }

    // Picks up ".reload config": the core reloads all config files (including playerbots.conf)
    // into sConfigMgr before firing this hook. The initial (reload == false) call happens before
    // PlayerbotAIConfig::Initialize() has run, so only act on actual reloads - OnStartup covers
    // the initial load.
    void OnAfterConfigLoad(bool reload) override
    {
        if (!reload)
            return;

        sPlayerbotAIConfig.LoadRandomBotLevelConfig();
        RandomBotLevelMgr::instance().LoadConfig();
        LOG_INFO("playerbots", "[RandomBotLevelMgr] Level management config reloaded.");
    }

    void OnUpdate(uint32 diff) override
    {
        RandomBotLevelMgr::instance().Update(diff);
    }
};

// =============================================================================
// PLAYER SCRIPT
// =============================================================================
class RandomBotLevelPlayerScript : public PlayerScript
{
public:
    RandomBotLevelPlayerScript()
        : PlayerScript("RandomBotLevelPlayerScript",
              { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LOGOUT })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!sPlayerbotAIConfig.resetBotLevelEnabled)
            return;
        RandomBotLevelMgr::instance().OnBotLogin(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!sPlayerbotAIConfig.resetBotLevelEnabled)
            return;
        RandomBotLevelMgr::instance().OnBotLevelChanged(player, oldLevel);
    }

    void OnPlayerLogout(Player* player) override
    {
        RandomBotLevelMgr::instance().OnPlayerLogout(player);
    }
};

// -----------------------------------------------------------------------------
// ENTRY POINT
// -----------------------------------------------------------------------------
void AddSC_randombot_level_mgr()
{
    new RandomBotLevelWorldScript();
    new RandomBotLevelPlayerScript();
}
