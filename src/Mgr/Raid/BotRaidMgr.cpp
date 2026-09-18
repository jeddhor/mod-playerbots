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
    {624, "Vault of Archavon", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    {249, "Onyxia's Lair", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    {603, "Ulduar", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    {631, "Icecrown Citadel", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
    {724, "The Ruby Sanctum", 80, 25, RAID_DIFFICULTY_25MAN_NORMAL},
};

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
        {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            if (auto const itr = _runs.find(groupGuid); itr != _runs.end())
                name = itr->second.name;
        }

        LOG_INFO("playerbots", "[Raid] {} has run for {} minutes without finishing; sending the raid home", name,
                 sPlayerbotAIConfig.raidMaxMinutes);

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
        if (Group* group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter()))
            group->Disband(true);

        std::unique_lock<std::shared_mutex> lock(_mutex);
        _runs.erase(groupGuid);
        ++_finished;
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
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        for (auto const& [groupGuid, run] : _runs)
            running.insert(run.mapId);
    }

    for (RaidDef const& def : candidates)
    {
        if (running.count(def.mapId))
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

        if (PlayerbotAI::IsTank(bot))
            tanks.push_back(bot);
        else if (PlayerbotAI::IsHeal(bot))
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

std::string BotRaidMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);

    std::string detail;
    for (auto const& [groupGuid, run] : _runs)
    {
        detail += Acore::StringFormat("\n  {} ({} man, level {}), running for {} minute(s)", run.name, run.size,
                                      run.level, GetMSTimeDiffToNow(run.startedMs) / (MINUTE * IN_MILLISECONDS));
    }

    if (detail.empty())
        detail = "\n  nothing running";

    return Acore::StringFormat(
        "raids: {} started, {} finished, {} sent home for running too long, {} passes with too few free bots.\n"
        "{} of at most {} running now:{}",
        _started, _finished, _abandoned, _tooFewBots, _runs.size(), sPlayerbotAIConfig.raidMaxConcurrent, detail);
}
