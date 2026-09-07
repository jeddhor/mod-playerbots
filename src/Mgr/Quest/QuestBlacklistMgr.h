/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTBLACKLISTMGR_H
#define PLAYERBOTS_QUESTBLACKLISTMGR_H

#include "Define.h"
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

/**
 * Realm-wide record of which quests the NewRpg system keeps failing to progress.
 *
 * This replaces PlayerbotAI::lowPriorityQuest, a per-bot in-memory std::unordered_set. With that,
 * 3000 bots each independently burned five minutes discovering that the same quest was unworkable,
 * and every bot threw the knowledge away on logout.
 *
 * A quest is only skipped once it has failed `AiPlayerbot.Quest.BlacklistFailThreshold` times AND
 * no bot has ever completed it. That second condition matters: without it, one bot having a bad
 * pathing run would blacklist a quest that hundreds of other bots complete without trouble.
 *
 * Backed by `playerbot_quest_blacklist` in the playerbots database. Reads are frequent (the
 * DO_QUEST availability check walks the whole quest log), writes are rare, so the map is guarded by
 * a shared_mutex and DB writes go out asynchronously.
 */
class QuestBlacklistMgr
{
public:
    static QuestBlacklistMgr& instance()
    {
        static QuestBlacklistMgr instance;
        return instance;
    }

    /// Load the table into memory. Call once, during world startup.
    void Load();

    /// True if bots should currently avoid picking this quest as a DO_QUEST target.
    bool IsBlacklisted(uint32 questId);

    /// A bot gave up on this quest after exhausting its POI retries.
    void ReportFailure(uint32 questId);

    /// A bot turned this quest in. Clears any accumulated failure verdict.
    void ReportSuccess(uint32 questId);

    /**
     * True if this quest starts an escort.
     *
     * There is no QUEST_SPECIAL_FLAGS_ESCORT to test, so the set is resolved once at startup from
     * the two places escorts are actually defined: `script_waypoint` (the C++ npc_escortAI paths)
     * and `smart_scripts` rows that start a waypoint path on quest accept. Roughly fifty quests
     * realm-wide, so an immutable set built at load costs nothing to consult.
     *
     * Bots do not attempt escorts: they require holding a leash range on a moving NPC through
     * scripted ambushes, and failure is silent -- the NPC despawns and the quest fails with no
     * signal the bot can act on.
     */
    bool IsEscortQuest(uint32 questId) const;

    /**
     * How many other quests name this one as their prerequisite.
     *
     * Counted inbound rather than read from `NextQuestID`, which is populated on only 577 quests
     * against 4720 that declare a `PrevQuestID`. Counting the references that actually exist gives
     * the real branching factor; reading the sparse forward pointer would call most chain heads
     * dead ends.
     *
     * This is what lets a bot follow a zone's storyline instead of doing scattered one-offs: a quest
     * that opens three more is worth more than one that opens none, even at equal level.
     */
    uint32 GetUnlockCount(uint32 questId) const;

private:
    QuestBlacklistMgr() = default;
    ~QuestBlacklistMgr() = default;

    QuestBlacklistMgr(QuestBlacklistMgr const&) = delete;
    QuestBlacklistMgr& operator=(QuestBlacklistMgr const&) = delete;

    struct Record
    {
        uint32 failCount{0};
        uint32 successCount{0};
    };

    using BlacklistSet = std::unordered_set<uint32>;

    /// Recompute the read-side snapshot. Must be called with `_mutex` held for writing.
    void RebuildBlacklistSet();

    /// Resolve escort quests from the world DB. Called once by Load(); immutable afterwards.
    void LoadEscortQuests();

    /// Count, for every quest, how many quests list it as their prerequisite. Immutable after load.
    void LoadQuestChains();

    std::unordered_map<uint32, uint32> _unlockCounts;

    // Written once during Load and only read afterwards, so it needs no synchronisation.
    std::unordered_set<uint32> _escortQuests;

    // `_records` is the bookkeeping copy and is only ever touched on the (rare) write path.
    std::shared_mutex _mutex;
    std::unordered_map<uint32, Record> _records;

    // The read path is a lock-free snapshot instead of a shared_lock.
    //
    // IsBlacklisted() is called for all 25 quest-log slots inside both
    // CheckRpgStatusAvailable(RPG_DO_QUEST) and RandomChangeStatus's DO_QUEST branch, so with a
    // few hundred bots every idle tick funnelled thousands of shared_lock acquisitions through one
    // global mutex. Writes are rare (a quest failing or being turned in), so copy-on-write costs
    // nothing that matters and readers become an atomic load plus a hash lookup.
    std::atomic<std::shared_ptr<BlacklistSet const>> _blacklisted;
};

#define sQuestBlacklistMgr QuestBlacklistMgr::instance()

#endif
