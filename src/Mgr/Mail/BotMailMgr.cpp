/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotMailMgr.h"
#include "BotEventLogMgr.h"

#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringFormat.h"

bool BotMailMgr::Collect(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return false;

    // Take money and items from every mail, regardless of who sent it. See the class comment for
    // why CheckMailAction cannot be reused: it drops anything not sent by a connected non-bot
    // player, which is every auction payment.
    //
    // Mirrors WorldSession::HandleMailTakeItem's persistence exactly. The first version of this
    // moved items into the bags in memory only -- no transaction, no Mail::RemoveItem, no
    // removedItems, no _SaveMail. The item therefore ended up in the bot's bags *and* still
    // attached to its mail row in the database, which is genuine item duplication: two owners for
    // one item_instance. It showed up as auction-won mail whose item was also sitting in the
    // winner's inventory.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    uint32 collected = 0;
    uint32 money = 0;
    std::vector<uint32> emptied;
    std::vector<uint32> takenItems;
    std::vector<Mail const*> partiallyTaken;

    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->state == MAIL_STATE_DELETED)
            continue;

        // Undelivered mail is not the bot's to take yet.
        if (mail->deliver_time > GameTime::GetGameTime().count())
            continue;

        bool changed = false;

        if (mail->money)
        {
            money += mail->money;
            bot->ModifyMoney(static_cast<int32>(mail->money));
            mail->money = 0;
            changed = true;
        }

        // Copied, because Mail::RemoveItem mutates the very vector being walked.
        MailItemInfoVec const attachments = mail->items;

        bool itemsPending = false;
        for (MailItemInfo const& att : attachments)
        {
            Item* item = bot->GetMItem(att.item_guid);
            if (!item)
                continue;

            ItemPosCountVec dest;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
            {
                // Leave it attached rather than destroying it; the bot will come back with space.
                itemsPending = true;
                continue;
            }

            mail->RemoveItem(att.item_guid);
            bot->RemoveMItem(att.item_guid);
            takenItems.push_back(att.item_guid);

            // Without this the item cannot be removed from the bags later on.
            item->SetState(ITEM_UNCHANGED);
            bot->MoveItemToInventory(dest, item, true);

            ++collected;
            changed = true;
        }

        if (itemsPending)
        {
            if (changed)
            {
                mail->state = MAIL_STATE_CHANGED;
                partiallyTaken.push_back(mail);
            }
            continue;
        }

        mail->state = MAIL_STATE_DELETED;
        emptied.push_back(mail->messageID);
    }

    // Player::_SaveMail would do all of this, but it is protected and only WorldSession may call
    // it, so the statements are issued directly -- the same approach CheckMailAction already takes.
    for (uint32 itemGuid : takenItems)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM);
        stmt->SetData(0, itemGuid);
        trans->Append(stmt);
    }

    for (uint32 id : emptied)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
        stmt->SetData(0, id);
        trans->Append(stmt);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
        stmt->SetData(0, id);
        trans->Append(stmt);
    }

    // Mail that gave up its money but still holds items the bags had no room for: persist the
    // zeroed money so a restart cannot pay the bot twice.
    for (Mail const* mail : partiallyTaken)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_MAIL);
        stmt->SetData(0, uint8(!mail->items.empty()));
        stmt->SetData(1, uint32(mail->expire_time));
        stmt->SetData(2, uint32(mail->deliver_time));
        stmt->SetData(3, mail->money);
        stmt->SetData(4, mail->COD);
        stmt->SetData(5, uint8(mail->checked));
        stmt->SetData(6, mail->messageID);
        trans->Append(stmt);
    }

    // Save the bags and gold only when something actually moved into them. A bot holding nothing
    // but undelivered mail would otherwise write its whole inventory every interval for no reason.
    if (money || collected)
        bot->SaveInventoryAndGoldToDB(trans);

    // Commit whenever anything was queued at all -- deleting an emptied mail counts.
    //
    // Gating the commit on money-or-items skipped precisely the mails that have neither, and those
    // are the ones that sit in a mailbox forever keeping the minimap icon lit. Worse, the loop below
    // removes them from memory regardless: the rows would have stayed in the database while
    // vanishing from the running server, and come back at the next login.
    if (money || collected || !emptied.empty() || !partiallyTaken.empty())
        CharacterDatabase.CommitTransaction(trans);

    for (uint32 id : emptied)
    {
        bot->SendMailResult(id, MAIL_DELETED, MAIL_OK);
        bot->RemoveMail(id);
    }

    if (money || collected || !emptied.empty())
    {
        // Mails cleared is reported separately from items taken because the two failures look
        // nothing alike: a bot that takes items but never clears the empties still has a lit
        // minimap icon, which is the symptom that started this.
        LOG_DEBUG("playerbots", "[Logistics] {} collected {} copper and {} item(s) from mail, cleared {} mail(s)",
                  bot->GetName(), money, collected, emptied.size());

        sBotEventLogMgr.Record(bot, BotEventLogMgr::Cat::Mail,
                               Acore::StringFormat("read mail: {} item(s), {} mail(s) cleared", collected,
                                                   uint32(emptied.size())),
                               int64(money));

        std::unique_lock<std::shared_mutex> guard(instance()._mutex);
        ++instance()._collections;
        instance()._itemsCollected += collected;
        instance()._moneyCollected += money;
        instance()._mailsCleared += uint32(emptied.size());
    }

    return money || collected || !emptied.empty();
}

void BotMailMgr::Update(Player* bot, uint32 diff)
{
    if (!bot || !bot->IsInWorld())
        return;

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        uint32& timer = _timers[bot->GetGUID()];

        timer += diff;
        if (timer < sPlayerbotAIConfig.mailCollectIntervalMs)
            return;

        timer = 0;
    }

    // Cheap when there is nothing to do, which is the common case: no query, no transaction.
    if (bot->GetMails().empty())
        return;

    Collect(bot);
}

std::string BotMailMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat(
        "mail: {} collections, {} item(s) and {} copper taken, {} mail(s) cleared, {} bots tracked.\n"
        "If the count stops climbing while bots still hold mail, the timer is running but "
        "collection is failing -- most likely full bags.",
        _collections, _itemsCollected, _moneyCollected, _mailsCleared, _timers.size());
}
