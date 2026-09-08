/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotInspectorMgr.h"

#include "AccountMgr.h"
#include "Chat.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "Timer.h"

#include <algorithm>
#include <sstream>

namespace
{
    constexpr char const* PROTOCOL = "PBI";
    constexpr uint32 PROTOCOL_VERSION = 1;

    // Addon messages cap out around 255 bytes. Leave room for the envelope -- prefix, verb, key and
    // the seq/total counters -- so a full row can never push a chunk over the limit.
    constexpr size_t MAX_PAYLOAD = 180;

    constexpr uint32 FIND_LIMIT = 40;
    constexpr uint32 FIND_MIN_CHARS = 3;

    std::string Escape(std::string const& in)
    {
        // Tabs separate fields and newlines are not addon-message safe, so neither may appear in a
        // value. Names cannot contain them in practice, but a zone or item name from the DBCs is not
        // something to take on trust.
        std::string out = in;
        std::replace(out.begin(), out.end(), '\t', ' ');
        std::replace(out.begin(), out.end(), '\n', ' ');
        return out;
    }
}

bool BotInspectorMgr::IsAllowed(Player* sender) const
{
    if (!sender || !sPlayerbotAIConfig.inspectorEnabled)
        return false;

    return sender->GetSession() &&
           sender->GetSession()->GetSecurity() >= sPlayerbotAIConfig.inspectorMinSecurity;
}

bool BotInspectorMgr::RateLimit(Player* sender)
{
    uint32 const account = sender->GetSession()->GetAccountId();
    uint32 const now = getMSTime();

    std::lock_guard<std::mutex> guard(_mutex);

    auto itr = _lastRequestMs.find(account);
    if (itr != _lastRequestMs.end() && GetMSTimeDiffToNow(itr->second) < sPlayerbotAIConfig.inspectorMinIntervalMs)
        return false;

    _lastRequestMs[account] = now;
    return true;
}

void BotInspectorMgr::Reply(Player* to, std::string const& verb, std::string const& key,
                            std::vector<std::string> const& rows)
{
    // Pack rows into as few messages as possible, then send each with its sequence number. The
    // client assembles on (verb, key) and knows it is finished when seq == total.
    std::vector<std::string> chunks;
    std::string current;

    for (std::string const& row : rows)
    {
        if (!current.empty() && current.size() + row.size() + 1 > MAX_PAYLOAD)
        {
            chunks.push_back(current);
            current.clear();
        }

        if (!current.empty())
            current += '\t';

        current += row;
    }

    // An empty result still gets one chunk, so the addon can tell "nothing here" apart from a
    // response that never arrived.
    chunks.push_back(current);

    uint32 const total = static_cast<uint32>(chunks.size());
    for (uint32 i = 0; i < total; ++i)
    {
        std::string const payload = Acore::StringFormat("{}\t{}\t{}\t{}\t{}\t{}\t{}", PROTOCOL, PROTOCOL_VERSION,
                                                        verb, key, i + 1, total, chunks[i]);

        // CHAT_MSG_WHISPER, not CHAT_MSG_ADDON.
        //
        // CHAT_MSG_ADDON is the *event* the client raises after it recognises addon traffic; it is
        // not a channel, and the 3.3.5 client has no inbound case for it. Sending it crashed the
        // client outright with ERROR #134 the moment a reply arrived.
        //
        // The inbound direction already told us the right answer and I did not read it across:
        // ChatHandler accepts LANG_ADDON only on WHISPER, PARTY, RAID, GUILD and BATTLEGROUND. The
        // language marks the traffic as addon data; the type says which channel carried it. A reply
        // whispered back from the player to themselves mirrors exactly what the addon sent.
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, payload.c_str(), LANG_ADDON, CHAT_TAG_NONE,
                                     to->GetGUID(), to->GetName(), to->GetGUID(), to->GetName());
        to->SendDirectMessage(&data);
    }
}

void BotInspectorMgr::SendError(Player* to, uint32 code, std::string const& text)
{
    Reply(to, "ERR", std::to_string(code), {Escape(text)});
}

void BotInspectorMgr::HandleZones(Player* to)
{
    // Zone id and bot count only. Small enough that the whole realm fits in a handful of messages,
    // which is the point: the addon can draw its zone tree without fetching a single bot.
    std::unordered_map<uint32, uint32> counts;

    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBotsRef())
        if (bot && bot->IsInWorld() && sRandomPlayerbotMgr.IsRandomBot(bot))
            ++counts[bot->GetZoneId()];

    std::vector<std::string> rows;
    rows.reserve(counts.size());
    for (auto const& [zoneId, count] : counts)
        rows.push_back(Acore::StringFormat("{}:{}", zoneId, count));

    Reply(to, "ZONES", "0", rows);
}

void BotInspectorMgr::HandleList(Player* to, uint32 zoneId)
{
    std::vector<std::string> rows;

    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBotsRef())
    {
        if (!bot || !bot->IsInWorld() || bot->GetZoneId() != zoneId)
            continue;

        // Listing an altbot would expose a player's alt by name and level even without DETAIL.
        if (!sRandomPlayerbotMgr.IsRandomBot(bot))
            continue;

        // guid:name:level:class:race:gold
        rows.push_back(Acore::StringFormat("{}:{}:{}:{}:{}:{}", guid.GetCounter(), Escape(bot->GetName()),
                                           bot->GetLevel(), static_cast<uint32>(bot->getClass()),
                                           static_cast<uint32>(bot->getRace()), bot->GetMoney() / 10000));
    }

    Reply(to, "LIST", std::to_string(zoneId), rows);
}

void BotInspectorMgr::HandleFind(Player* to, std::string const& needle)
{
    if (needle.size() < FIND_MIN_CHARS)
    {
        SendError(to, 400, "search needs at least 3 characters");
        return;
    }

    std::string lower = needle;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    std::vector<std::string> rows;
    uint32 matched = 0;

    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBotsRef())
    {
        if (!bot || !bot->IsInWorld() || !sRandomPlayerbotMgr.IsRandomBot(bot))
            continue;

        std::string name = bot->GetName();
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);

        if (name.find(lower) == std::string::npos)
            continue;

        ++matched;

        // Capped, and the count of matches is reported separately, so the addon can say "40 of 312"
        // rather than implying it showed everything.
        if (rows.size() < FIND_LIMIT)
            rows.push_back(Acore::StringFormat("{}:{}:{}:{}", guid.GetCounter(), Escape(bot->GetName()),
                                               bot->GetLevel(), bot->GetZoneId()));
    }

    rows.push_back(Acore::StringFormat("#matched:{}", matched));
    Reply(to, "FIND", needle, rows);
}

void BotInspectorMgr::HandleDetail(Player* to, ObjectGuid::LowType botGuid, std::string const& section)
{
    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));

    if (!bot || !bot->IsInWorld())
    {
        SendError(to, 404, "bot not online");
        return;
    }

    // The gate that matters, and it is narrower than "is a bot".
    //
    // GET_PLAYERBOT_AI is true for altbots and selfbots as well as random bots, and an altbot is a
    // human player's own alt being driven by the AI. Dumping its gear, quests and gold is inspecting
    // that player, not debugging the realm -- the same line we declined to cross for ordinary
    // characters, reached by a different route.
    //
    // Random bots are server-owned and have no such claim, so those are the only ones answered for.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        SendError(to, 403, "not a random bot -- altbots and selfbots belong to a player");
        return;
    }

    std::vector<std::string> rows;
    std::string const key = Acore::StringFormat("{}:{}", botGuid, section);

    if (section == "CORE")
    {
        rows.push_back(Acore::StringFormat("level:{}", bot->GetLevel()));
        rows.push_back(Acore::StringFormat("xp:{}:{}", bot->GetUInt32Value(PLAYER_XP),
                                           bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)));
        rows.push_back(Acore::StringFormat("hp:{}:{}", bot->GetHealth(), bot->GetMaxHealth()));
        rows.push_back(Acore::StringFormat("mana:{}:{}", bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA)));
        rows.push_back(Acore::StringFormat("str:{}", static_cast<uint32>(bot->GetStat(STAT_STRENGTH))));
        rows.push_back(Acore::StringFormat("agi:{}", static_cast<uint32>(bot->GetStat(STAT_AGILITY))));
        rows.push_back(Acore::StringFormat("sta:{}", static_cast<uint32>(bot->GetStat(STAT_STAMINA))));
        rows.push_back(Acore::StringFormat("int:{}", static_cast<uint32>(bot->GetStat(STAT_INTELLECT))));
        rows.push_back(Acore::StringFormat("spi:{}", static_cast<uint32>(bot->GetStat(STAT_SPIRIT))));
        rows.push_back(Acore::StringFormat("armor:{}", bot->GetArmor()));
        rows.push_back(Acore::StringFormat("gold:{}", bot->GetMoney()));
        rows.push_back(Acore::StringFormat("zone:{}", bot->GetZoneId()));
    }
    else if (section == "GEAR")
    {
        // Item ids only. The client resolves names, icons and stats from its own cache, which is the
        // single largest saving in the whole protocol and why gear fits in a couple of messages.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            rows.push_back(Acore::StringFormat("{}:{}:{}", slot, item->GetEntry(),
                                               item->GetItemRandomPropertyId()));
        }
    }
    else if (section == "SKILL")
    {
        for (uint32 skill : {SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
                             SKILL_HERBALISM, SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING,
                             SKILL_MINING, SKILL_SKINNING, SKILL_TAILORING, SKILL_COOKING, SKILL_FIRST_AID,
                             SKILL_FISHING})
        {
            if (!bot->HasSkill(skill))
                continue;

            rows.push_back(Acore::StringFormat("{}:{}:{}", skill, bot->GetSkillValue(skill),
                                               bot->GetPureMaxSkillValue(skill)));
        }
    }
    else if (section == "QUEST")
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;

            rows.push_back(Acore::StringFormat("{}:{}", questId, static_cast<uint32>(bot->GetQuestStatus(questId))));
        }
    }
    else
    {
        SendError(to, 400, "unknown section");
        return;
    }

    Reply(to, "DETAIL", key, rows);
}

bool BotInspectorMgr::HandleMessage(Player* sender, std::string const& msg)
{
    if (msg.rfind(std::string(PROTOCOL) + "\t", 0) != 0)
        return false;  // not ours

    LOG_DEBUG("playerbots", "[Inspector] request from {}: {}", sender ? sender->GetName() : "?", msg);

    if (!IsAllowed(sender))
    {
        // Answered rather than ignored: an addon that gets silence cannot tell "refused" from
        // "server does not have this feature", and the operator would be debugging the wrong thing.
        if (sender && sPlayerbotAIConfig.inspectorEnabled)
            SendError(sender, 403, "insufficient privileges");
        return true;
    }

    if (!RateLimit(sender))
        return true;

    std::vector<std::string> parts;
    std::istringstream stream(msg);
    std::string field;
    while (std::getline(stream, field, '\t'))
        parts.push_back(field);

    // PBI <version> <VERB> [args...]
    if (parts.size() < 3)
        return true;

    std::string const& verb = parts[2];

    if (verb == "ZONES")
        HandleZones(sender);
    else if (verb == "LIST" && parts.size() >= 4)
        HandleList(sender, static_cast<uint32>(std::strtoul(parts[3].c_str(), nullptr, 10)));
    else if (verb == "FIND" && parts.size() >= 4)
        HandleFind(sender, parts[3]);
    else if (verb == "DETAIL" && parts.size() >= 5)
        HandleDetail(sender, static_cast<ObjectGuid::LowType>(std::strtoul(parts[3].c_str(), nullptr, 10)),
                     parts[4]);
    else
        SendError(sender, 400, "unknown verb");

    return true;
}
