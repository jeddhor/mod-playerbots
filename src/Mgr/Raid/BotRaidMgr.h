/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTRAIDMGR_H
#define PLAYERBOTS_BOTRAIDMGR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <map>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

/**
 * Raids that bots run by themselves.
 *
 * The module already knows how to fight a raid: there are boss strategies for nineteen of them, from
 * Molten Core to Icecrown Citadel, and PlayerbotAI attaches the right one from the map id on entry.
 * What has never existed is everything before the first pull. Bots self-organise into parties of at
 * most five -- the grouping code's own ceiling -- the Dungeon Finder only ever forms a five, and the
 * one place that converts a group to a raid is reachable only from a chat command. So a raid happened
 * when a person built one by hand, and otherwise not at all. All that scripted boss AI was unused.
 *
 * This is the missing half: assemble a raid from bots that are standing around at the right level,
 * put it inside, and let the machinery that already exists take it from there. Once the group is in
 * the instance with a bot leading, BotDungeonMgr's autopilot drives the pulls exactly as it does for
 * a five-man -- its own gate is "inside an instance, led by a bot", and a raid map satisfies it.
 *
 * Who is eligible is the other half of the answer, and it is why this arrives with the era caps:
 * a raid needs twenty-five or forty characters that are all at the same level and are going to stay
 * there. That is precisely what an era-capped population is.
 */
class BotRaidMgr
{
public:
    static BotRaidMgr& instance()
    {
        static BotRaidMgr instance;
        return instance;
    }

    /// Read the raid entry points out of the world database. Called once at startup.
    void Load();

    /// World-thread tick: retire finished raids, and start one if there is room and bodies for it.
    void Update(uint32 diff);

    std::string DescribeStats() const;

    /// Record a raid member's death and what killed it. Called from the death hooks; a death outside any
    /// tracked raid is ignored.
    void NoteMemberDeath(Player* victim, std::string const& killer);

    /// Whether this group is a raid this manager put together. The random bot lifecycle asks, because
    /// it otherwise pulls apart any bot group led by a bot.
    bool IsManagedGroup(ObjectGuid groupGuid) const;

    /// One raid this manager knows how to run. Public because the table of them lives in the
    /// implementation file, where it is readable next to the reasoning about what belongs in it.
    struct RaidDef
    {
        uint32 mapId;
        char const* name;
        uint8 level;            //< the era population that runs it: 60, 70 or 80
        uint32 size;            //< bodies to bring, which is also the map's own limit
        Difficulty difficulty;  //< 25 man normal where the raid has sizes, otherwise the only one
    };

private:
    /// Where the entrance's area trigger puts a player, straight from the world database.
    struct EntryPoint
    {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        float o{0.0f};
        bool valid{false};
    };

    /// A raid currently in progress.
    struct Run
    {
        uint32 mapId{0};
        uint8 level{0};
        uint32 size{0};
        uint32 startedMs{0};
        std::string name;
        /// The instance's own completed-encounter mask, as last seen. Bit indexes are
        /// DungeonEncounter.dbc boss numbers, which is what the client is told too.
        uint32 encounterMask{0};
        /// How many encounters this raid has killed, and how many the instance has to offer.
        uint32 bossKills{0};
        uint32 encounterTotal{0};
        /// What has been killing the raid, by name, and how many died in total. A run that kills nothing
        /// says nothing about why on its own; this is the why.
        std::map<std::string, uint32> deaths;
        uint32 deathCount{0};
    };

    /// Read each running raid's encounter mask out of its instance, so a kill is noticed and a run can
    /// be judged on what it actually killed rather than on how long it lasted.
    void SampleEncounters();

    /// Retire raids that have finished, emptied out, or run out of time.
    void PruneRuns();

    /// Try to put one raid together. False when nothing was started.
    bool TryStartRaid();

    /// Assemble and place a raid of this shape for this faction. False when it could not be filled.
    bool StartRaid(RaidDef const& def, TeamId team);

    /// A bot free to be taken for a raid: a random bot, idle, alive, ungrouped, out in the world.
    static bool IsAvailable(Player* bot, RaidDef const& def, TeamId team);

    /// The raids this realm is configured to run, in the order the table declares them.
    std::vector<RaidDef> EnabledRaids() const;

    mutable std::shared_mutex _mutex;
    uint32 _nextCheckMs{0};
    std::unordered_map<ObjectGuid, Run> _runs;
    /// When each raid may be attempted again, by map id. A raid the bots cannot survive would otherwise
    /// be retried every minute, killing another twenty-five of them each time.
    std::unordered_map<uint32, uint32> _retryAfterMs;
    /// When each member's death was last recorded. Both death hooks fire for one death -- the creature one
    /// names the killer, the catch-all covers falls and drownings -- so the second is ignored.
    std::unordered_map<ObjectGuid, uint32> _lastDeathMs;
    std::unordered_map<uint32, EntryPoint> _entryPoints;

    uint32 _started{0};
    uint32 _finished{0};
    uint32 _abandoned{0};
    uint32 _tooFewBots{0};
};

#define sBotRaidMgr BotRaidMgr::instance()

#endif
