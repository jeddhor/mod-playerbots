/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotEventLogMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "StringFormat.h"

#include <algorithm>
#include <ctime>

namespace
{
/// Tabs separate fields on the wire and colons separate this row's own fields, so neither may
/// appear in free text. Item and creature names come from the DBCs, which is not somewhere to take
/// punctuation on trust.
std::string Sanitize(std::string const& in)
{
    std::string out = in;
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == ':')
            c = ' ';
    return out;
}
}  // namespace

void BotEventLogMgr::Record(Player* player, Cat cat, std::string const& text, int64 deltaCopper)
{
    // Self bots only. Callers are scattered through vendoring, mail, auctions and death handling,
    // and making each of them ask this question first would be the sort of duplication that is
    // eventually got wrong in one place.
    if (!player || !IsSelfBot(player))
        return;

    std::unique_lock<std::shared_mutex> guard(_mutex);

    Log& log = _logs[player->GetGUID()];

    Event ev;
    ev.seq = log.nextSeq++;
    ev.when = uint32(time(nullptr));
    ev.cat = cat;
    ev.balance = int64(player->GetMoney());
    ev.delta = deltaCopper;
    ev.text = Sanitize(text);

    log.events.push_back(std::move(ev));

    // Oldest out first. A session view is worth having cheaply; a complete one is not worth an
    // unbounded buffer per played character.
    while (log.events.size() > MAX_EVENTS)
        log.events.pop_front();
}

std::vector<std::string> BotEventLogMgr::GetSince(Player* player, uint32 afterSeq, uint32 limit) const
{
    std::vector<std::string> rows;
    if (!player || !limit)
        return rows;

    std::shared_lock<std::shared_mutex> guard(_mutex);

    auto itr = _logs.find(player->GetGUID());
    if (itr == _logs.end())
        return rows;

    Log const& log = itr->second;

    // Oldest first, so the client appends in order and its own scrollback reads chronologically.
    for (Event const& ev : log.events)
    {
        if (ev.seq <= afterSeq)
            continue;

        rows.push_back(Acore::StringFormat("{}:{}:{}:{}:{}:{}", ev.seq, ev.when, char(ev.cat), ev.balance,
                                           ev.delta, ev.text));

        if (rows.size() >= limit)
            break;
    }

    return rows;
}

void BotEventLogMgr::Forget(ObjectGuid guid)
{
    std::unique_lock<std::shared_mutex> guard(_mutex);
    _logs.erase(guid);
}
