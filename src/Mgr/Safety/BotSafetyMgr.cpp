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
}

bool BotSafetyMgr::IsOutOfWorld(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->FindMap())
        return false;

    // The core's own test, used in Player::UpdateUnderwaterState: below the map's floor means
    // outside the world rather than merely underground.
    return bot->GetPositionZ() < bot->GetMap()->GetMinHeight(bot->GetPositionX(), bot->GetPositionY());
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

    if (IsOutOfWorld(bot))
    {
        Anchor anchor;
        {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            auto itr = _anchors.find(guid);
            if (itr != _anchors.end())
                anchor = itr->second;
        }

        // Restore to the last place the bot was demonstrably standing on something. That is a far
        // better destination than a graveyard: it is where the bot was working, so whatever it was
        // doing survives the recovery.
        if (anchor.valid && anchor.mapId == bot->GetMapId())
        {
            LOG_INFO("playerbots", "[Safety] {} fell out of the world at z={:.1f}, restoring to its last safe ground",
                     bot->GetName(), bot->GetPositionZ());

            bot->TeleportTo(anchor.mapId, anchor.pos.GetPositionX(), anchor.pos.GetPositionY(),
                            anchor.pos.GetPositionZ(), anchor.pos.GetOrientation());
            ++_recoveries;
        }
        else
        {
            // No anchor yet -- the bot fell before it was ever seen on solid ground. The graveyard
            // is a poor destination but an available one, and still better than dying.
            if (GraveyardStruct const* graveyard =
                    sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId(), bot->InBattleground()))
            {
                LOG_INFO("playerbots", "[Safety] {} fell out of the world with no safe ground recorded, "
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
        "Fall recoveries: {} restored to safe ground, {} sent to a graveyard for want of one. "
        "Anchors held: {}.\n"
        "A recovery count that keeps climbing means the movement cause is still there; this only "
        "catches the symptom.",
        _recoveries, _recoveriesNoAnchor, _anchors.size());
}
