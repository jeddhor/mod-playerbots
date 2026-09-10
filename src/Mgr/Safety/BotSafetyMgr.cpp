/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotSafetyMgr.h"

#include "Log.h"
#include "Map.h"
#include "GameGraveyard.h"
#include "GameTime.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringFormat.h"

namespace
{
    // How close to the ground counts as standing on it. Generous, because a bot mid-jump or on a
    // slope is still somewhere sane to come back to.
    constexpr float SAFE_GROUND_TOLERANCE = 5.0f;

    /// Two failures closer together than this are the same failure recurring, not two failures.
    constexpr float SAME_SPOT_RADIUS = 25.0f;

    /// After this many recoveries to the same place, the anchor is part of the problem.
    ///
    /// Restoring a bot to where it was last standing is the right first answer, and it works for
    /// the great majority: 18 of 45 bots in one run needed exactly one recovery. But when the
    /// anchor sits somewhere the bot immediately slides back under, repeating it is worse than
    /// useless -- it pins the bot in a loop for the rest of the run and buries the log. Three
    /// attempts is enough to establish that this particular place does not work.
    constexpr uint32 ANCHOR_DISTRUST_AFTER = 3;
}

bool BotSafetyMgr::IsOutOfWorld(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->FindMap())
        return false;

    // The core's own test, used in Player::UpdateUnderwaterState: below the map's floor means
    // outside the world rather than merely underground.
    return bot->GetPositionZ() < bot->GetMap()->GetMinHeight(bot->GetPositionX(), bot->GetPositionY());
}

/**
 * Below the world's surface with nothing underneath.
 *
 * IsOutOfWorld only catches a bot beneath the map's *floor*, which sits hundreds of yards below the
 * terrain -- so a bot walking along under the landscape, which is what an operator actually sees and
 * calls "under the map", never tripped it. Zero recoveries had ever been logged while exactly that
 * was happening.
 *
 * The difficulty is telling that apart from a bot legitimately below the surface: in a cave, a mine,
 * or the lower floor of a building. Two conditions together do it honestly:
 *
 *   - the bot is below the highest surface at its position, which a cave bot also is; and
 *   - there is nothing solid beneath it, which a cave bot never is -- it is standing on the cave.
 *
 * A flying or falling-from-height bot fails the first test, since it is *above* the surface, so this
 * needs no special case for flight.
 */
bool BotSafetyMgr::IsUnderTerrain(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return false;

    Map* map = bot->FindMap();
    if (!map)
        return false;

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();

    // The highest walkable surface here -- terrain or building roof.
    float const top = map->GetHeight(bot->GetPhaseMask(), x, y, MAX_HEIGHT, true);
    if (top <= INVALID_HEIGHT || z >= top - UNDER_TERRAIN_TOLERANCE)
        return false;

    // Anything solid beneath, searching down from just above the bot. A cave floor answers here;
    // the void does not.
    float const below = map->GetHeight(bot->GetPhaseMask(), x, y, z + 2.0f, true);
    return below <= INVALID_HEIGHT;
}

bool BotSafetyMgr::IsOnSafeGround(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->FindMap())
        return false;

    if (bot->IsFalling() || bot->IsInFlight() || bot->IsBeingTeleported())
        return false;

    float const ground = bot->GetMap()->GetHeight(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                                                  bot->GetPositionZ());
    if (ground <= INVALID_HEIGHT)
        return false;

    return std::fabs(bot->GetPositionZ() - ground) <= SAFE_GROUND_TOLERANCE;
}

void BotSafetyMgr::Update(Player* bot, uint32 diff)
{
    if (!bot || !sPlayerbotAIConfig.safetyRecoverFromFalls)
        return;

    ObjectGuid const guid = bot->GetGUID();

    // Checked on a timer rather than every update: the map height query is not free, and a bot that
    // has fallen out of the world stays fallen for the second it takes to notice.
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        Anchor& anchor = _anchors[guid];

        anchor.timer += diff;
        if (anchor.timer < sPlayerbotAIConfig.safetyCheckIntervalMs)
            return;

        anchor.timer = 0;
    }

    if (IsOutOfWorld(bot) || IsUnderTerrain(bot))
    {
        Anchor anchor;
        bool trustAnchor = false;
        {
            std::unique_lock<std::shared_mutex> lock(_mutex);
            Anchor& stored = _anchors[guid];

            // Same place as last time, or a fresh incident? Distance decides, not a timer: a bot
            // that slid back under within a second and a bot that fell somewhere else an hour
            // later are different problems and must not share a counter.
            bool const sameSpot = stored.consecutive > 0 && stored.lastFailureMap == bot->GetMapId() &&
                                  stored.lastFailure.GetExactDist2d(bot->GetPositionX(), bot->GetPositionY()) <
                                      SAME_SPOT_RADIUS;

            stored.consecutive = sameSpot ? stored.consecutive + 1 : 1;
            stored.lastFailureMap = bot->GetMapId();
            stored.lastFailure.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

            trustAnchor = stored.valid && stored.mapId == bot->GetMapId() &&
                          stored.consecutive < ANCHOR_DISTRUST_AFTER;

            if (!trustAnchor && stored.valid && stored.consecutive >= ANCHOR_DISTRUST_AFTER)
            {
                // The anchor leads back here. Drop it so a new one is recorded wherever the bot
                // ends up, rather than teleporting it into the same hole a fourth time.
                LOG_INFO("playerbots",
                         "[Safety] {} has needed {} recoveries within {:.0f} yards of "
                         "({:.0f},{:.0f},{:.1f}) on map {} -- its safe ground leads straight back "
                         "under, so it is being sent to a graveyard instead",
                         bot->GetName(), stored.consecutive, SAME_SPOT_RADIUS, bot->GetPositionX(),
                         bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());

                stored.valid = false;
                ++_anchorsDistrusted;
            }

            anchor = stored;
        }

        // Restore to the last place the bot was demonstrably standing on something. That is a far
        // better destination than a graveyard: it is where the bot was working, so whatever it was
        // doing survives the recovery.
        if (trustAnchor)
        {
            // Enough detail to find the cause next time. "fell at z=-1012" says only that it
            // happened; where, on which map, and what the bot was trying to do is what makes the
            // next one diagnosable rather than another twelve hour wait.
            // Which of the two tests fired matters: "below the map's floor" and "walking along
            // under the landscape" have different causes, and the second was invisible until the
            // under-terrain test existed. Reporting them identically would waste that distinction.
            LOG_INFO("playerbots",
                     "[Safety] {} {} at ({:.0f},{:.0f},{:.1f}) map {} zone {} "
                     "[{}selfbot, activity {}], restoring to its last safe ground",
                     bot->GetName(), IsOutOfWorld(bot) ? "fell out of the world" : "went under the terrain",
                     bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
                     bot->GetZoneId(), IsSelfBot(bot) ? "" : "not a ",
                     GET_PLAYERBOT_AI(bot) ? int(GET_PLAYERBOT_AI(bot)->rpgInfo.GetStatus()) : -1);

            bot->TeleportTo(anchor.mapId, anchor.pos.GetPositionX(), anchor.pos.GetPositionY(),
                            anchor.pos.GetPositionZ(), anchor.pos.GetOrientation());
            ++_recoveries;
        }
        else
        {
            // Either no anchor was ever recorded, or the one we had has just been distrusted.
            // The graveyard is a poor destination but an available one, and still better than
            // leaving the bot under the map or letting it die there.
            if (GraveyardStruct const* graveyard =
                    sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId(), bot->InBattleground()))
            {
                LOG_INFO("playerbots", "[Safety] {} has no usable safe ground, "
                                       "sending to the nearest graveyard",
                         bot->GetName());

                bot->TeleportTo(graveyard->Map, graveyard->x, graveyard->y, graveyard->z, bot->GetOrientation());
                ++_recoveriesNoAnchor;
            }
        }

        // Cancel the fall so the core does not apply falling damage on arrival. Recovering a bot and
        // then killing it for the fall it was rescued from would defeat the entire point.
        bot->SetFallInformation(GameTime::GetGameTime().count(), bot->GetPositionZ());
        return;
    }

    if (IsOnSafeGround(bot))
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        Anchor& anchor = _anchors[guid];
        anchor.mapId = bot->GetMapId();
        anchor.pos.Relocate(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation());
        anchor.valid = true;

        // Standing on something again ends the streak. A bot that recovers and then works normally
        // for a while has had one incident, not the beginning of a loop.
        anchor.consecutive = 0;
    }
}

void BotSafetyMgr::Forget(ObjectGuid guid)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _anchors.erase(guid);
}

std::string BotSafetyMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return Acore::StringFormat(
        "Fall recoveries: {} restored to safe ground, {} sent to a graveyard for want of one, "
        "{} anchors abandoned for leading straight back under. Anchors held: {}.\n"
        "A recovery count that keeps climbing means the movement cause is still there; this only "
        "catches the symptom.",
        _recoveries, _recoveriesNoAnchor, _anchorsDistrusted, _anchors.size());
}
