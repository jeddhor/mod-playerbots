/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotRaidMgr.h"

#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFGMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "InstanceScript.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"

#include <algorithm>
#include <random>
#include <unordered_set>

namespace
{
/**
 * The raids bots can run, and which era population runs each.
 *
 * Only raids the module actually has boss strategies for: putting a raid together for a map whose
 * fights nothing knows how to handle produces forty bots standing in a doorway.
 *
 * Size is the map's own limit as well as the number to bring. A twenty-five man map refuses the
 * twenty-sixth player, so "bring exactly this many" and "the raid is full" are the same number, and
 * the difficulty has to agree with it -- entering at ten man difficulty and then trying to seat
 * twenty-five is the same mistake in a different place.
 *
 * Levels are the era ceilings, not the map's minimum. A raid wants a whole roster at one level, and
 * that is what an era-capped population is; the minimums in dungeon_access_template are lower and
 * would let a level 60 group walk into Karazhan and die in it.
 */
constexpr BotRaidMgr::RaidDef RAID_TABLE[] = {
    // Classic, for the level 60 population.
    {509, "Ruins of Ahn'Qiraj", 60, 20, RAID_DIFFICULTY_10MAN_NORMAL},
    {409, "Molten Core", 60, 40, RAID_DIFFICULTY_10MAN_NORMAL},
    {469, "Blackwing Lair", 60, 40, RAID_DIFFICULTY_10MAN_NORMAL},

    // The Burning Crusade, for the level 70 population.
    {532, "Karazhan", 70, 10, RAID_DIFFICULTY_10MAN_NORMAL},
    {568, "Zul'Aman", 70, 10, RAID_DIFFICULTY_10MAN_NORMAL},
    {565, "Gruul's Lair", 70, 25, RAID_DIFFICULTY_10MAN_NORMAL},
    {544, "Magtheridon's Lair", 70, 25, RAID_DIFFICULTY_10MAN_NORMAL},
    {548, "Serpentshrine Cavern", 70, 25, RAID_DIFFICULTY_10MAN_NORMAL},
    {550, "Tempest Keep", 70, 25, RAID_DIFFICULTY_10MAN_NORMAL},
    {534, "Battle for Mount Hyjal", 70, 25, RAID_DIFFICULTY_10MAN_NORMAL},
    {564, "Black Temple", 70, 25, RAID_DIFFICULTY_10MAN_NORMAL},

    // Wrath, for the level 80 population. These are the ones with a real difficulty choice.
    {533, "Naxxramas", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    {615, "The Obsidian Sanctum", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    {616, "The Eye of Eternity", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    // Vault of Archavon is deliberately absent. Entry is gated on which faction holds Wintergrasp, so the
    // teleport is simply refused most of the time: a raid was assembled, reported as placed, and found a
    // minute later with all twenty-five members alive and standing in Wintergrasp instead of inside.
    {249, "Onyxia's Lair", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    {603, "Ulduar", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    {631, "Icecrown Citadel", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    {724, "The Ruby Sanctum", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
};

/// A run that ends this soon is reported in detail: it finished nothing, and where its members ended
/// up is the only way to tell "nobody arrived" from "everybody died".
constexpr uint32 EARLY_EXIT_MS = 5 * MINUTE * IN_MILLISECONDS;

/// Tanks wanted for a raid of this size: one per ten bodies, never fewer than one.
uint32 TanksFor(uint32 size)
{
    return std::max<uint32>(1, size / 10);
}

/// Healers wanted for a raid of this size: one per five bodies, never fewer than two.
uint32 HealersFor(uint32 size)
{
    return std::max<uint32>(2, size / 5);
}

/// The worst few things that killed a raid, most frequent first, as one readable clause.
std::string DescribeDeaths(std::map<std::string, uint32> const& deaths, uint32 total)
{
    if (!total)
        return "nobody died";

    std::vector<std::pair<std::string, uint32>> ranked(deaths.begin(), deaths.end());
    std::sort(ranked.begin(), ranked.end(),
              [](auto const& a, auto const& b) { return a.second > b.second; });

    std::string out = Acore::StringFormat("{} death(s)", total);
    uint32 named = 0;
    for (auto const& [who, count] : ranked)
    {
        if (named++ >= 4)
            break;

        out += Acore::StringFormat("{} {} x{}", named == 1 ? ":" : ",", who, count);
    }

    return out;
}
}  // namespace

void BotRaidMgr::Load()
{
    // Entry points come from the same area triggers a player walks through, so a bot arrives where a
    // person would and not at a hand-typed coordinate that drifts when the map data changes.
    QueryResult result = WorldDatabase.Query(
        "SELECT target_map, target_position_x, target_position_y, target_position_z, target_orientation "
        "FROM areatrigger_teleport");

    uint32 loaded = 0;
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const mapId = fields[0].Get<uint16>();

            bool wanted = false;
            for (RaidDef const& def : RAID_TABLE)
                if (def.mapId == mapId)
                    wanted = true;

            if (!wanted)
                continue;

            // First trigger for a map wins. Several raids have more than one entrance and they all
            // arrive in the same room.
            if (_entryPoints.count(mapId))
                continue;

            EntryPoint point;
            point.x = fields[1].Get<float>();
            point.y = fields[2].Get<float>();
            point.z = fields[3].Get<float>();
            point.o = fields[4].Get<float>();
            point.valid = true;
            _entryPoints[mapId] = point;
            ++loaded;
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "[Raid] Loaded entry points for {} of {} raids", loaded,
             uint32(std::size(RAID_TABLE)));

    for (RaidDef const& def : RAID_TABLE)
        if (!_entryPoints.count(def.mapId))
            LOG_INFO("playerbots", "[Raid] {} (map {}) has no entrance area trigger and will be skipped",
                     def.name, def.mapId);
}

std::vector<BotRaidMgr::RaidDef> BotRaidMgr::EnabledRaids() const
{
    std::vector<RaidDef> enabled;

    for (RaidDef const& def : RAID_TABLE)
    {
        // No entrance, no raid. Reported once at load.
        if (!_entryPoints.count(def.mapId))
            continue;

        // An empty map list means every raid in the table; otherwise it is a whitelist.
        if (!sPlayerbotAIConfig.raidMaps.empty() &&
            std::find(sPlayerbotAIConfig.raidMaps.begin(), sPlayerbotAIConfig.raidMaps.end(), def.mapId) ==
                sPlayerbotAIConfig.raidMaps.end())
            continue;

        enabled.push_back(def);
    }

    return enabled;
}

bool BotRaidMgr::IsAvailable(Player* bot, RaidDef const& def, TeamId team)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        return false;

    // Random bots only. A person's own characters are not raid filler, and neither is a bot somebody
    // is currently playing with.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (!GET_PLAYERBOT_AI(bot))
        return false;

    if (bot->GetTeamId() != team)
        return false;

    // Exactly at the era level. A level 74 bot in a level 70 raid is a bot that will not be there
    // tomorrow, and one below the level is a liability to the twenty-four others.
    if (bot->GetLevel() != def.level)
        return false;

    // Busy with something a raid must not interrupt.
    if (bot->GetGroup() || bot->InBattleground() || bot->IsInCombat() || bot->IsInFlight())
        return false;

    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        return false;

    if (Map* map = bot->FindMap(); map && map->Instanceable())
        return false;

    if (bot->IsBeingTeleported() || !bot->GetSession() || bot->GetSession()->isLogingOut())
        return false;

    return true;
}

void BotRaidMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.raidEnabled)
        return;

    uint32 const now = getMSTime();
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        if (_nextCheckMs && now < _nextCheckMs)
            return;
    }
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _nextCheckMs = now + sPlayerbotAIConfig.raidIntervalMs;
    }

    SampleEncounters();
    PruneRuns();

    size_t active = 0;
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        active = _runs.size();
    }

    if (active >= sPlayerbotAIConfig.raidMaxConcurrent)
        return;

    TryStartRaid();

    (void)diff;
}

/**
 * What the raid has actually killed.
 *
 * Until this, a run was judged by how long it lasted, which says nothing: the log read the same for a
 * raid that cleared two bosses and one that wiped on the first pull and stood around as ghosts. The
 * instance already keeps the answer -- InstanceScript holds the completed-encounter mask it reports to
 * the client, with one bit per DungeonEncounter.dbc boss -- so this reads it rather than trying to
 * infer kills from creature deaths, which would need a list of every boss in nineteen raids and would
 * be wrong the moment a script named one differently.
 */
void BotRaidMgr::SampleEncounters()
{
    struct Observation
    {
        ObjectGuid groupGuid;
        uint32 mask;
        uint32 total;
        std::string name;
    };

    std::vector<Observation> seen;

    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        for (auto const& [groupGuid, run] : _runs)
        {
            Group* group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
            if (!group)
                continue;

            // Any member who is actually inside will do: they share the instance.
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || !member->IsInWorld() || member->GetMapId() != run.mapId)
                    continue;

                Map* map = member->FindMap();
                InstanceMap* instanceMap = map ? map->ToInstanceMap() : nullptr;
                InstanceScript* script = instanceMap ? instanceMap->GetInstanceScript() : nullptr;
                if (!script)
                    break;

                uint32 total = 0;
                if (DungeonEncounterList const* encounters =
                        sObjectMgr->GetDungeonEncounterList(run.mapId, map->GetDifficulty()))
                    total = uint32(encounters->size());

                seen.push_back({groupGuid, script->GetCompletedEncounterMask(), total, run.name});
                break;
            }
        }
    }

    for (Observation const& observation : seen)
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        auto const itr = _runs.find(observation.groupGuid);
        if (itr == _runs.end())
            continue;

        Run& run = itr->second;
        run.encounterTotal = observation.total;

        if (observation.mask == run.encounterMask)
            continue;

        // Newly set bits are kills since the last look. Counted rather than assumed to be one, because
        // a pass is a minute apart and a raid can finish two fights in that time.
        uint32 const added = observation.mask & ~run.encounterMask;
        uint32 killed = 0;
        for (uint32 bit = 0; bit < 32; ++bit)
            if (added & (1u << bit))
                ++killed;

        run.encounterMask = observation.mask;
        run.bossKills += killed;

        LOG_INFO("playerbots", "[Raid] {} has killed {} of {} boss encounter(s)", observation.name, run.bossKills,
                 run.encounterTotal ? std::to_string(run.encounterTotal) : std::string("?"));
    }
}

void BotRaidMgr::PruneRuns()
{
    std::vector<ObjectGuid> finished;
    std::vector<ObjectGuid> expired;

    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        for (auto const& [groupGuid, run] : _runs)
        {
            Group* group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
            if (!group)
            {
                finished.push_back(groupGuid);
                continue;
            }

            // Nobody left inside. Either the raid was cleared and the bots walked out, or it wiped
            // and released; both mean this run is over and the group should not be left standing.
            uint32 inside = 0;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->IsInWorld() && member->GetMapId() == run.mapId)
                    ++inside;
            }

            if (!inside)
            {
                // A run that empties out in its first minutes did not finish anything, and the two
                // reasons look identical from here: nobody ever arrived, or everybody died and left.
                // So say where the members actually are. Gruul's Lair was reported as started with
                // twenty-five placed inside and was over forty-five seconds later, and there was no
                // way to tell those apart from the log.
                if (GetMSTimeDiffToNow(run.startedMs) < EARLY_EXIT_MS)
                {
                    std::string where;
                    uint32 alive = 0;
                    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    {
                        Player* member = ref->GetSource();
                        if (!member)
                            continue;

                        if (member->IsAlive())
                            ++alive;

                        if (where.size() < 120)
                            where += Acore::StringFormat("{}{}", where.empty() ? "" : ",", member->GetMapId());
                    }

                    LOG_INFO("playerbots",
                             "[Raid] {} emptied after {} second(s): {} of {} members alive, maps {} -- expected map {}",
                             run.name, GetMSTimeDiffToNow(run.startedMs) / IN_MILLISECONDS, alive,
                             group->GetMembersCount(), where.empty() ? "none" : where, run.mapId);
                }

                finished.push_back(groupGuid);
                continue;
            }

            if (GetMSTimeDiffToNow(run.startedMs) >= sPlayerbotAIConfig.raidMaxMinutes * MINUTE * IN_MILLISECONDS)
                expired.push_back(groupGuid);
        }
    }

    for (ObjectGuid const& groupGuid : expired)
    {
        std::string name;
        std::string deathReport;
        uint32 kills = 0;
        {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            if (auto const itr = _runs.find(groupGuid); itr != _runs.end())
            {
                name = itr->second.name;
                kills = itr->second.bossKills;
                deathReport = DescribeDeaths(itr->second.deaths, itr->second.deathCount);
            }
        }

        LOG_INFO("playerbots",
                 "[Raid] {} has run for {} minutes without finishing ({} boss encounter(s) killed, {}); sending the "
                 "raid home",
                 name, sPlayerbotAIConfig.raidMaxMinutes, kills, deathReport);

        if (Group* group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter()))
        {
            // Out of the instance first, then disbanded. A bot left standing inside a raid map with no
            // group is a bot with nothing to do and no way to leave.
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->IsInWorld() && member->GetMap()->Instanceable())
                    member->TeleportTo(member->m_homebindMapId, member->m_homebindX, member->m_homebindY,
                                       member->m_homebindZ, 0.0f);
            }

            group->Disband(true);
        }

        std::unique_lock<std::shared_mutex> lock(_mutex);
        _runs.erase(groupGuid);
        ++_abandoned;
    }

    for (ObjectGuid const& groupGuid : finished)
    {
        std::string name;
        uint32 mapId = 0;
        uint32 minutes = 0;
        uint32 kills = 0;
        uint32 total = 0;
        std::string deathReport;
        bool early = false;

        {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            if (auto const itr = _runs.find(groupGuid); itr != _runs.end())
            {
                name = itr->second.name;
                mapId = itr->second.mapId;
                minutes = GetMSTimeDiffToNow(itr->second.startedMs) / (MINUTE * IN_MILLISECONDS);
                kills = itr->second.bossKills;
                total = itr->second.encounterTotal;
                deathReport = DescribeDeaths(itr->second.deaths, itr->second.deathCount);
                early = GetMSTimeDiffToNow(itr->second.startedMs) < EARLY_EXIT_MS;
            }
        }

        // Logged, because the end of a raid is the interesting half. A run that started leaves a line
        // and a run that was sent home leaves a line; without this one a raid that emptied out looked
        // exactly like a raid still quietly in progress, and the only way to tell was to count group
        // members by hand.
        if (!name.empty())
        {
            // Said in terms of what was killed, not how long it took. A cleared raid and a wipe both end
            // with an empty instance, and only this number tells them apart.
            bool const cleared = total && kills >= total;
            LOG_INFO("playerbots",
                     "[Raid] {} is over after {} minute(s): {} of {} boss encounter(s) killed -- {}. {}", name,
                     minutes, kills, total ? std::to_string(total) : std::string("?"),
                     cleared ? "cleared" : (kills ? "gave up part way" : "killed nothing"), deathReport);
        }

        if (Group* group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter()))
            group->Disband(true);

        std::unique_lock<std::shared_mutex> lock(_mutex);
        _runs.erase(groupGuid);
        ++_finished;

        // Hold a raid back before trying it again, and hold it back much longer when the group did not
        // last. Every run so far has ended the same way -- a wipe, with the ghosts appearing outside --
        // and with one raid at a time and a pass every minute that means another twenty-five bots
        // killed and re-geared every few minutes. A raid the bots cannot survive should be attempted
        // occasionally, not continuously, and the wait lets the idle bots go to a different one.
        if (mapId)
            _retryAfterMs[mapId] =
                getMSTime() + (early ? sPlayerbotAIConfig.raidRetryMinutes : 1) * MINUTE * IN_MILLISECONDS;
    }
}

bool BotRaidMgr::TryStartRaid()
{
    std::vector<RaidDef> candidates = EnabledRaids();
    if (candidates.empty())
        return false;

    // Shuffled so a realm does not run the first row of the table forever.
    static std::mt19937 rng(std::random_device{}());
    std::shuffle(candidates.begin(), candidates.end(), rng);

    // Which raids are already running, so two raids of the same instance are not started at once --
    // the second would be a separate instance of the same fights, which is not what more raiding
    // means.
    std::unordered_set<uint32> running;
    std::unordered_set<uint32> waiting;
    {
        uint32 const now = getMSTime();
        std::shared_lock<std::shared_mutex> lock(_mutex);
        for (auto const& [groupGuid, run] : _runs)
            running.insert(run.mapId);

        for (auto const& [mapId, retryAt] : _retryAfterMs)
            if (now < retryAt)
                waiting.insert(mapId);
    }

    for (RaidDef const& def : candidates)
    {
        if (running.count(def.mapId) || waiting.count(def.mapId))
            continue;

        for (TeamId team : {TEAM_ALLIANCE, TEAM_HORDE})
        {
            if (StartRaid(def, team))
                return true;
        }
    }

    std::unique_lock<std::shared_mutex> lock(_mutex);
    ++_tooFewBots;
    return false;
}

bool BotRaidMgr::StartRaid(RaidDef const& def, TeamId team)
{
    EntryPoint entry;
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        if (auto const itr = _entryPoints.find(def.mapId); itr != _entryPoints.end())
            entry = itr->second;
    }

    if (!entry.valid)
        return false;

    std::vector<Player*> tanks;
    std::vector<Player*> healers;
    std::vector<Player*> damage;

    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBotsRef())
    {
        if (!IsAvailable(bot, def, team))
            continue;

        // By spec, not by the strategies the bot is carrying right now.
        //
        // Asked the default way, these two report what a bot is *doing*: whether its engine currently
        // holds a tank or heal strategy. A bot out in the world questing alone is doing damage
        // whatever it was built for, so no healer was ever found -- every pass reported zero of the
        // five it needed, while tanks turned up because plate classes carry a tank strategy while
        // soloing anyway. What matters for recruiting is what the bot's talents make it, which is
        // what it will go back to being when the instance rebuilds its strategies on entry.
        if (PlayerbotAI::IsTank(bot, true))
            tanks.push_back(bot);
        else if (PlayerbotAI::IsHeal(bot, true))
            healers.push_back(bot);
        else
            damage.push_back(bot);
    }

    uint32 const wantTanks = TanksFor(def.size);
    uint32 const wantHealers = HealersFor(def.size);

    if (tanks.size() < wantTanks || healers.size() < wantHealers ||
        tanks.size() + healers.size() + damage.size() < def.size)
    {
        LOG_DEBUG("playerbots",
                  "[Raid] not enough {} bots free for {}: {} tanks of {}, {} healers of {}, {} others, {} needed",
                  team == TEAM_ALLIANCE ? "Alliance" : "Horde", def.name, tanks.size(), wantTanks, healers.size(),
                  wantHealers, damage.size(), def.size);
        return false;
    }

    // Roles first, then anyone. A raid that is filled to size before it has its healers is a raid
    // that wipes on the first pull, and the whole point of taking twenty-five bots out of the world
    // is that the run goes somewhere.
    std::vector<Player*> roster;
    roster.reserve(def.size);

    auto take = [&roster, &def](std::vector<Player*>& from, uint32 count)
    {
        for (uint32 i = 0; i < count && !from.empty() && roster.size() < def.size; ++i)
        {
            roster.push_back(from.back());
            from.pop_back();
        }
    };

    take(tanks, wantTanks);
    take(healers, wantHealers);
    take(damage, def.size);  // capped by roster.size() < def.size inside
    take(healers, def.size);
    take(tanks, def.size);

    if (roster.size() < def.size)
        return false;

    // A tank leads. The leader is the bot the dungeon autopilot puts in front, and the bot in front
    // should be the one built to be hit.
    Player* leader = roster.front();

    Group* group = new Group();
    if (!group->Create(leader))
    {
        delete group;
        LOG_INFO("playerbots", "[Raid] could not create a group for {}", def.name);
        return false;
    }

    sGroupMgr->AddGroup(group);

    // Converted before the sixth member is added, not after: a party holds five, and AddMember
    // refuses the rest.
    group->ConvertToRaid();
    group->SetRaidDifficulty(def.difficulty);

    uint32 seated = 1;
    for (size_t i = 1; i < roster.size(); ++i)
    {
        if (group->AddMember(roster[i]))
            ++seated;
    }

    if (seated < def.size)
    {
        LOG_INFO("playerbots", "[Raid] only seated {} of {} for {}; disbanding", seated, def.size, def.name);
        group->Disband(true);
        return false;
    }

    // Drop any dungeon queue a member was in the middle of joining.
    //
    // Eligibility skips a bot the Dungeon Finder already knows about, but a bot whose join is still
    // sitting unprocessed on its own session reads as unqueued -- the head-of-line gap BotLfgMgr keeps
    // its own pending list for. A bot was seen asking for a dungeon twenty-one seconds before this
    // manager recruited it into Karazhan, so the request has to be cancelled from here as well as
    // avoided. allowgroup is false deliberately: only this bot's own queue entries go, never anything
    // belonging to the raid group just built.
    for (Player* member : roster)
        sLFGMgr->LeaveAllLfgQueues(member->GetGUID(), false);

    uint32 placed = 0;
    for (Player* member : roster)
    {
        // Straight to the arrival point of the entrance trigger. Walking a raid across a continent is
        // a different feature and a much harder one; nobody can see this happen, because the only
        // place it is visible from is inside the instance.
        if (member->TeleportTo(def.mapId, entry.x, entry.y, entry.z, entry.o))
            ++placed;
    }

    if (!placed)
    {
        LOG_INFO("playerbots", "[Raid] nobody could be placed inside {}; disbanding", def.name);
        group->Disband(true);
        return false;
    }

    Run run;
    run.mapId = def.mapId;
    run.level = def.level;
    run.size = def.size;
    run.startedMs = getMSTime();
    run.name = def.name;

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _runs[group->GetGUID()] = run;
        ++_started;
    }

    LOG_INFO("playerbots", "[Raid] {} {} bots are running {} ({} man, level {}), led by {}; {} placed inside",
             seated, team == TEAM_ALLIANCE ? "Alliance" : "Horde", def.name, def.size, def.level, leader->GetName(),
             placed);

    return true;
}

/**
 * What killed a raider.
 *
 * A run that reports "nought of nine bosses" says nothing about why. It reads the same whether the raid
 * wiped on the first trash pack at the door or reached the third boss and lost to one mechanic, and those
 * want completely different fixes. The instance already tells us what was killed; this is the other half.
 *
 * Kept per run and named, because the name is the diagnosis: twenty-five deaths to "Coldflame" is a raid
 * standing in fire, twenty-five to "Lord Marrowgar" is a raid being out-damaged, and a spread across trash
 * names is a raid that never reached a boss at all.
 */
void BotRaidMgr::NoteMemberDeath(Player* victim, std::string const& killer)
{
    if (!victim)
        return;

    Group* group = victim->GetGroup();
    if (!group)
        return;

    std::unique_lock<std::shared_mutex> lock(_mutex);

    auto const itr = _runs.find(group->GetGUID());
    if (itr == _runs.end())
        return;

    // Only deaths inside the raid itself. A member killed on the way somewhere else is not this run's story.
    if (victim->GetMapId() != itr->second.mapId)
        return;

    // One death, not two. A creature kill fires both hooks, and whichever arrives first is the one kept:
    // usually the named creature, occasionally the catch-all naming whatever the bot was fighting, which
    // for a raid death is nearly always the same thing.
    uint32 const now = getMSTime();
    if (auto const seen = _lastDeathMs.find(victim->GetGUID()); seen != _lastDeathMs.end())
    {
        if (getMSTimeDiff(seen->second, now) < 3 * IN_MILLISECONDS)
            return;
    }

    _lastDeathMs[victim->GetGUID()] = now;

    ++itr->second.deathCount;
    ++itr->second.deaths[killer.empty() ? "something unnamed" : killer];
}

bool BotRaidMgr::IsManagedGroup(ObjectGuid groupGuid) const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _runs.count(groupGuid) != 0;
}

std::string BotRaidMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);

    std::string detail;
    for (auto const& [groupGuid, run] : _runs)
    {
        detail += Acore::StringFormat("\n  {} ({} man, level {}), running for {} minute(s), {} of {} boss(es) down",
                                      run.name, run.size, run.level,
                                      GetMSTimeDiffToNow(run.startedMs) / (MINUTE * IN_MILLISECONDS), run.bossKills,
                                      run.encounterTotal ? std::to_string(run.encounterTotal) : std::string("?"));
    }

    if (detail.empty())
        detail = "\n  nothing running";

    return Acore::StringFormat(
        "raids: {} started, {} finished, {} sent home for running too long, {} passes with too few free bots.\n"
        "{} of at most {} running now:{}",
        _started, _finished, _abandoned, _tooFewBots, _runs.size(), sPlayerbotAIConfig.raidMaxConcurrent, detail);
}
