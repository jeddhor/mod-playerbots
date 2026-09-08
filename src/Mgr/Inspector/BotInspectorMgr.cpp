/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotInspectorMgr.h"

#include "AccountMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "WorldSession.h"
#include "SpellMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "Timer.h"

#include <algorithm>
#include <map>
#include <set>
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

bool BotInspectorMgr::RateLimit(Player* sender, std::string const& verb, std::string const& section)
{
    // Ten tokens is a little over two bot selections back to back, which is what an operator
    // clicking through a roster actually does. Refill is one token per configured interval.
    constexpr float CAPACITY = 10.0f;

    float cost = 1.0f;
    if (verb == "FIND")
        cost = 3.0f;
    else if (verb == "DETAIL" && section == "RECIPE")
        cost = 5.0f;

    uint32 const account = sender->GetSession()->GetAccountId();
    uint32 const now = getMSTime();
    uint32 const interval = std::max<uint32>(1, sPlayerbotAIConfig.inspectorMinIntervalMs);

    std::lock_guard<std::mutex> guard(_mutex);

    Bucket& bucket = _buckets[account];
    if (bucket.lastMs == 0)
        bucket.tokens = CAPACITY;
    else
        bucket.tokens = std::min(CAPACITY, bucket.tokens + float(getMSTimeDiff(bucket.lastMs, now)) / float(interval));

    bucket.lastMs = now;

    if (bucket.tokens < cost)
        return false;

    bucket.tokens -= cost;
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
    {
        // The name has to come from here. Item ids resolve against the client's own cache, but a
        // 3.3.5 client has no Lua call that turns an area id into a name -- GetMapZones works off
        // map indices, not area ids, and nothing maps one to the other. Sending it costs about
        // fifteen bytes per zone and is the difference between "Stormwind City" and "zone 1519".
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        char const* name = area ? area->area_name[LOCALE_enUS] : nullptr;

        rows.push_back(Acore::StringFormat("{}:{}:{}", zoneId, count, Escape(name ? name : "?")));
    }

    // What this session may actually do, so the panel can enable its GM controls honestly rather
    // than offering buttons that fail. RBAC is the real gate on these commands -- not the security
    // level the inspector itself checks -- so ask about the exact permissions, not a proxy for them.
    WorldSession* session = to->GetSession();
    rows.push_back(Acore::StringFormat("#gm:{}:{}",
                   session && session->HasPermission(rbac::RBAC_PERM_COMMAND_APPEAR) ? 1 : 0,
                   session && session->HasPermission(rbac::RBAC_PERM_COMMAND_SUMMON) ? 1 : 0));

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
        // Identity is repeated here even though LIST carries it, because a bot can be selected
        // straight out of FIND without its zone roster ever being fetched. A detail pane that can
        // only label itself when you arrived by one particular route is a bug waiting to happen.
        rows.push_back(Acore::StringFormat("name:{}", bot->GetName()));
        rows.push_back(Acore::StringFormat("class:{}", static_cast<uint32>(bot->getClass())));
        rows.push_back(Acore::StringFormat("race:{}", static_cast<uint32>(bot->getRace())));
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
        rows.push_back(Acore::StringFormat("res:{}:{}:{}:{}:{}",
                                           bot->GetResistance(SPELL_SCHOOL_FIRE),
                                           bot->GetResistance(SPELL_SCHOOL_NATURE),
                                           bot->GetResistance(SPELL_SCHOOL_FROST),
                                           bot->GetResistance(SPELL_SCHOOL_SHADOW),
                                           bot->GetResistance(SPELL_SCHOOL_ARCANE)));
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

            // Permanent enchant only. Temporary enchants and gems are noise for "why is this bot
            // underperforming"; the permanent one answers whether the bot ever enchanted at all.
            rows.push_back(Acore::StringFormat("{}:{}:{}:{}", slot, item->GetEntry(),
                                               item->GetItemRandomPropertyId(),
                                               item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)));
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
        // Distinct quest zones, emitted once each as "#z:<id>:<name>" header rows. The client groups
        // by zone but cannot name one: the same area-id problem as ZONES, and QuestSort names are
        // not loaded from DBC at all in this core, so negative sorts (class, seasonal, profession
        // quests) collapse to a single "Other" bucket rather than being invented.
        std::map<uint32, std::string> questZones;

        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // Objective progress is the reason this section exists. "Bot has quest 1234" tells you
            // nothing; "bot has killed 0/8 for six hours" tells you the spawn is unreachable, the
            // mob is tapped by something else, or the bot is standing in the wrong place entirely.
            std::string objectives;

            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                if (!quest->RequiredNpcOrGo[i] || !quest->RequiredNpcOrGoCount[i])
                    continue;

                if (!objectives.empty())
                    objectives += ',';

                objectives += Acore::StringFormat("{}/{}", bot->GetQuestSlotCounter(slot, i),
                                                  quest->RequiredNpcOrGoCount[i]);
            }

            // Item objectives are counted from the bag rather than the quest log: the four slot
            // counters track creatures and objects only, and reading them for an item objective
            // reports a confident zero forever.
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                if (!quest->RequiredItemId[i] || !quest->RequiredItemCount[i])
                    continue;

                if (!objectives.empty())
                    objectives += ',';

                objectives += Acore::StringFormat("{}/{}", bot->GetItemCount(quest->RequiredItemId[i], true),
                                                  quest->RequiredItemCount[i]);
            }

            if (objectives.empty())
                objectives = "-";

            // The title has to travel. Item ids resolve against the client's own cache, but there is
            // no client-side database of quests it has never taken, and no Lua call to ask the
            // server for one -- so unlike GEAR, sending only ids here would render 25 blank lines.
            //
            // Title goes last because quest names contain colons ("Bring Me Shackles!: ..." and
            // friends) and the client rejoins everything past this point.
            int32 const sort = quest->GetZoneOrSort();
            uint32 const questZone = sort > 0 ? static_cast<uint32>(sort) : 0;

            if (questZones.find(questZone) == questZones.end())
            {
                AreaTableEntry const* area = questZone ? sAreaTableStore.LookupEntry(questZone) : nullptr;
                char const* name = area ? area->area_name[LOCALE_enUS] : nullptr;
                questZones[questZone] = name ? name : "Other";
            }

            rows.push_back(Acore::StringFormat("{}:{}:{}:{}:{}:{}", questId,
                                               static_cast<uint32>(bot->GetQuestStatus(questId)),
                                               quest->GetQuestLevel(), questZone, objectives,
                                               quest->GetTitle()));
        }

        for (auto const& [zoneId, name] : questZones)
            rows.push_back(Acore::StringFormat("#z:{}:{}", zoneId, Escape(name)));
    }
    else if (section == "RECIPE")
    {
        // The one response big enough to misbehave, which is why the client fetches it only when
        // its tab is opened. A maxed crafter with two professions knows several hundred recipes.
        //
        // Spell ids only. Unlike quests, the client can name any spell it is handed -- every spell
        // is in its own Spell.dbc -- so this is the "ids, never names" rule working as intended,
        // and it is what keeps a 300-recipe list to a few thousand bytes.
        constexpr uint32 MAX_RECIPES = 400;

        static std::set<uint32> const professions = {
            SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
            SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_TAILORING,
            SKILL_COOKING, SKILL_FIRST_AID
        };

        uint32 found = 0;
        bool truncated = false;

        for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
        {
            if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
                continue;

            // Every class ability has a skill line too, so the filter is on the skill being a
            // crafting profession -- without it this would return the bot's entire spellbook.
            SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
            for (auto itr = bounds.first; itr != bounds.second; ++itr)
            {
                SkillLineAbilityEntry const* ability = itr->second;
                if (!ability || professions.find(ability->SkillLine) == professions.end())
                    continue;

                if (found >= MAX_RECIPES)
                {
                    truncated = true;
                    break;
                }

                // skill : spell : required rank : rank at which it stops giving skill-ups. The last
                // lets the client grey out trivial recipes exactly as the trade skill window does.
                rows.push_back(Acore::StringFormat("{}:{}:{}:{}", ability->SkillLine, spellId,
                                                   ability->MinSkillLineRank,
                                                   ability->TrivialSkillLineRankHigh));
                ++found;
                break;   // one row per spell, even where several skill lines teach it
            }

            if (truncated)
                break;
        }

        if (truncated)
            rows.push_back(Acore::StringFormat("#more:{}", MAX_RECIPES));
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

    std::vector<std::string> parts;
    std::istringstream stream(msg);
    std::string field;
    while (std::getline(stream, field, '\t'))
        parts.push_back(field);

    // PBI <version> <VERB> [args...]
    if (parts.size() < 3)
        return true;

    std::string const& verb = parts[2];

    // Parsed before the limiter runs, because the limiter prices requests by verb and a RECIPE
    // fetch must not cost the same as a zone listing.
    if (!RateLimit(sender, verb, parts.size() >= 5 ? parts[4] : std::string()))
    {
        // Answered, not dropped. A silent rejection is indistinguishable at the client from a
        // server with no inspector at all, and that mistaken diagnosis has already cost time here.
        SendError(sender, 429, "rate limited -- slow down");
        return true;
    }

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
