/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTINTEGRITYMGR_H
#define PLAYERBOTS_QUESTINTEGRITYMGR_H

#include "Define.h"

class Player;

/**
 * Repairs quest log entries stored as QUEST_STATUS_COMPLETE whose objectives are not actually met.
 *
 * Such a record is a lie that two different code paths answer differently, which is what makes it
 * so hard to spot from inside the game:
 *
 *   - The questgiver marker and the bot's own turn-in planning read the *stored* status, so both
 *     believe the quest is ready to hand in. The NPC wears a yellow '?' and the bot walks to it.
 *   - The actual hand-in re-derives completion from the objectives (Player::CanRewardQuest), finds
 *     them unmet, and refuses.
 *
 * The bot then arrives, is turned away, and has no way to learn anything from that -- its stored
 * status still says the quest is complete, so it goes straight back. That is a permanent stall, not
 * a slow retry, and it holds the character in place for as long as the bad record survives.
 *
 * Bad records were produced by a quest force-completion path fixed in 264df29a. The fix stopped new
 * ones being written but could not repair the several hundred already in character_queststatus, and
 * nothing in the running server ever re-checks a stored completion. This does, once per login.
 */
class QuestIntegrityMgr
{
public:
    /**
     * Re-validate every stored completion in `player`'s quest log, demoting any that does not hold
     * up to QUEST_STATUS_INCOMPLETE.
     *
     * Safe for real players as well as bots: a legitimately complete quest re-validates and is left
     * alone, so the worst this can do to an honest quest log is read it.
     *
     * @return how many quests were demoted.
     */
    static uint32 RepairStoredCompletions(Player* player);
};

#endif
