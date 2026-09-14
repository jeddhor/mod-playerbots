/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotInspectorMgr.h"
#include "BotEventLogMgr.h"
#include "NewRpgInfo.h"
#include "MapMgr.h"

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
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "PlayerbotMgr.h"
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

    // A party holds five, one of which is the player, so four is the most alts that can ever join.
    // Enforced server-side as well as in the panel: the panel's count is a convenience, not a rule.
    constexpr uint32 MAX_PARTY_ALTS = 4;

    // How long to keep retrying a queued invite. A bot login normally lands within a couple of
    // seconds; well past that it has failed, and holding the entry forever would mean a bot that
    // logs in much later gets yanked into a party the player has long since stopped thinking about.
    constexpr uint32 INVITE_WAIT_MS = 30000;

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

    // Reclaim buckets for accounts that have stopped asking. One entry per account that ever used
    // the inspector is small, but it is unbounded over an uptime measured in weeks, and a map that
    // only ever grows is the kind of thing that is fine until it is not. Swept opportunistically so
    // there is no timer to own.
    if (_buckets.size() > 64)
    {
        constexpr uint32 IDLE_MS = 10 * 60 * 1000;
        for (auto itr = _buckets.begin(); itr != _buckets.end();)
        {
            if (itr->first != account && getMSTimeDiff(itr->second.lastMs, now) > IDLE_MS)
                itr = _buckets.erase(itr);
            else
                ++itr;
        }
    }

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

namespace
{
/**
 * The strategies that make a bot actually play a role.
 *
 * Generous on purpose: Engine::addStrategy silently ignores a name its class never registered, so
 * one string per role can name both the generic strategies ("tank assist") and the class ones
 * ("tank", "heal") without knowing which class it is being applied to. A rogue told to tank simply
 * gets the parts that exist for a rogue -- which is to say, almost nothing, correctly.
 *
 * Removals matter as much as additions. Without "-dps assist" a tank keeps choosing targets like a
 * damage dealer and the role change is cosmetic.
 */
struct RoleStrategies
{
    char const* combat;
    char const* nonCombat;
};

RoleStrategies const ROLE_TANK{
    "+tank,+tank assist,+tank face,+pull,-dps assist,-heal,-healer dps",
    "-save mana"};

RoleStrategies const ROLE_HEAL{
    "+heal,+healer dps,-tank,-tank assist,-tank face,-dps assist,-pull",
    "+save mana"};

RoleStrategies const ROLE_DPS{
    "+dps assist,+dps,+aoe,-tank,-tank assist,-tank face,-heal,-healer dps,-pull",
    "-save mana"};

/// What role a bot's current strategies amount to. Read from the engine, never guessed from spec.
char const* DescribeRole(PlayerbotAI* ai)
{
    if (!ai)
        return "-";

    if (ai->HasStrategy("tank assist", BotState::BOT_STATE_COMBAT) ||
        ai->HasStrategy("tank face", BotState::BOT_STATE_COMBAT))
        return "tank";

    if (ai->HasStrategy("heal", BotState::BOT_STATE_COMBAT))
        return "heal";

    if (ai->HasStrategy("dps assist", BotState::BOT_STATE_COMBAT))
        return "dps";

    return "none";
}
}  // namespace

void BotInspectorMgr::HandleAlts(Player* to)
{
    if (!to || !to->GetSession())
        return;

    // Straight from the character table rather than from anything in memory, because the whole
    // point is to show characters that are *not* logged in -- an offline alt exists nowhere else.
    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, name, level, class, race FROM characters "
        "WHERE account = {} AND deleteInfos_Account IS NULL ORDER BY level DESC, name ASC",
        to->GetSession()->GetAccountId());

    std::vector<std::string> rows;

    if (result)
    {
        Group const* group = to->GetGroup();

        do
        {
            Field* fields = result->Fetch();
            ObjectGuid::LowType const low = fields[0].Get<uint32>();

            // The character being played is not one of its own alts.
            if (low == to->GetGUID().GetCounter())
                continue;

            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(low);
            Player* const online = ObjectAccessor::FindPlayer(guid);

            // Four states, because the panel offers a different action for each: nothing to do for
            // one already in the party, "invite" for a bot that is up but elsewhere, "add" for an
            // offline alt, and nothing at all for one a human is presently playing.
            char const* state = "offline";
            if (online)
            {
                if (!GET_PLAYERBOT_AI(online))
                    state = "player";
                else if (group && group->IsMember(guid))
                    state = "party";
                else
                    state = "bot";
            }

            // Role is reported, not inferred client-side: only the server can see which strategies
            // an engine is actually carrying, and that is the honest answer to "what is this bot
            // doing right now" -- spec would only say what it was built for.
            char const* role = online ? DescribeRole(GET_PLAYERBOT_AI(online)) : "-";

            rows.push_back(Acore::StringFormat("{}:{}:{}:{}:{}:{}:{}", low, Escape(fields[1].Get<std::string>()),
                                               fields[2].Get<uint8>(), fields[3].Get<uint8>(),
                                               fields[4].Get<uint8>(), state, role));
        } while (result->NextRow());
    }

    // The requesting character itself, so the panel can offer self-bot controls beside the alts.
    // Prefixed rather than listed among them: it is not one of its own alts, and the panel needs to
    // tell it apart to draw a different set of buttons.
    PlayerbotAI* const selfAI = GET_PLAYERBOT_AI(to);
    rows.push_back(Acore::StringFormat("#self:{}:{}:{}:{}", to->GetGUID().GetCounter(), Escape(to->GetName()),
                                       IsSelfBot(to) ? 1 : 0, DescribeRole(selfAI)));

    Reply(to, "ALTS", "0", rows);
}

namespace
{
    /// Short, readable name for an RPG activity. The enum values mean nothing to a person.
    char const* DescribeRpgStatus(NewRpgStatus status)
    {
        switch (status)
        {
            case RPG_IDLE:          return "Idle";
            case RPG_GO_GRIND:      return "Heading to grind";
            case RPG_GO_CAMP:       return "Heading to camp";
            case RPG_WANDER_RANDOM: return "Wandering";
            case RPG_WANDER_NPC:    return "Visiting an NPC";
            case RPG_DO_QUEST:      return "Questing";
            case RPG_TRAVEL_FLIGHT: return "Flying";
            case RPG_REST:          return "Resting";
            case RPG_OUTDOOR_PVP:   return "Outdoor PvP";
            case RPG_VENDOR:        return "Vendoring";
            case RPG_MAILBOX:       return "At the mailbox";
            case RPG_GATHER:        return "Gathering";
            case RPG_TRAIN:         return "Training";
            case RPG_FISH:          return "Fishing";
            case RPG_CRAFT_GOAL:    return "Farming to craft";
            default:                return "Unknown";
        }
    }
}

void BotInspectorMgr::HandleSelfStat(Player* to)
{
    std::vector<std::string> rows;

    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(to);
    if (!botAI || !IsSelfBot(to))
    {
        // Answered rather than errored: the panel polls this, and an error would light up the
        // client's error banner every couple of seconds for a character that is simply not a bot.
        rows.push_back("off:1");
        Reply(to, "SELFSTAT", "0", rows);
        return;
    }

    NewRpgInfo& rpg = botAI->rpgInfo;
    NewRpgStatus const status = rpg.GetStatus();

    rows.push_back(Acore::StringFormat("act:{}", DescribeRpgStatus(status)));

    // Whether the character is fighting, which is not the same question as which activity it chose.
    //
    // The activity is set once and then persists for up to half an hour; combat runs on a different
    // engine entirely. A character being jumped repeatedly on its way to a gather route reports
    // "Gathering" for the whole run while never taking a step, and the display said so without ever
    // mentioning the fighting -- which reads as the activity being wrong rather than interrupted.
    rows.push_back(Acore::StringFormat("combat:{}", to->IsInCombat() ? 1 : 0));
    rows.push_back(Acore::StringFormat("secs:{}", GetMSTimeDiffToNow(rpg.startT) / 1000));

    // What the engine actually ran last tick. "Heading to grind" says what it intends; this says
    // what it did, and the two disagreeing is exactly the situation worth seeing.
    std::string const lastAction = botAI->GetLastAction(BOT_STATE_NON_COMBAT);
    if (!lastAction.empty())
        rows.push_back(Acore::StringFormat("action:{}", Escape(lastAction)));

    // Where it is walking, and how far up or down. Climb is reported separately from distance
    // because a flat four hundred yards and four hundred yards up a ridge are not the same walk.
    WorldPosition const& dest = rpg.moveFarPos;
    if (dest != WorldPosition() && dest.GetMapId() == to->GetMapId())
    {
        uint32 const destArea = sMapMgr->GetAreaId(to->GetPhaseMask(), dest);
        rows.push_back(Acore::StringFormat("dest:{}:{}:{}", destArea,
                                           uint32(to->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY())),
                                           int32(dest.GetPositionZ() - to->GetPositionZ())));
    }

    // What the server believes this character's speed is, for the mode it is actually in.
    //
    // Worth reporting because a self bot's client owns its own position, so the two can disagree:
    // a character seen outrunning a mount while the server still says 7 yd/s is a desync, and one
    // the server agrees is doing 25 is something having actually set its speed. Those need
    // different fixes and look identical from inside the game.
    UnitMoveType moveType = MOVE_RUN;
    char const* moveMode = "run";
    if (to->IsFlying())
    {
        moveType = MOVE_FLIGHT;
        moveMode = "flight";
    }
    else if (to->isSwimming())
    {
        moveType = MOVE_SWIM;
        moveMode = "swim";
    }
    else if (to->IsWalking())
    {
        moveType = MOVE_WALK;
        moveMode = "walk";
    }

    rows.push_back(Acore::StringFormat("speed:{:.1f}:{}:{}", to->GetSpeed(moveType),
                                       uint32(to->GetSpeedRate(moveType) * 100.0f), moveMode));

    rows.push_back(Acore::StringFormat("move:{}", to->isMoving() ? 1 : 0));
    rows.push_back(Acore::StringFormat("human:{}", botAI->HumanIsDriving() ? 1 : 0));

    // Stuck attempts are the single most useful number here. Five of them teleports the character,
    // and until now the only symptom was the character suddenly being somewhere else.
    rows.push_back(Acore::StringFormat("stuck:{}", rpg.stuckAttempts));

    if (Unit* victim = to->GetVictim())
    {
        uint32 const pct = victim->GetMaxHealth() ? uint32(victim->GetHealthPct()) : 0;
        rows.push_back(Acore::StringFormat("target:{}:{}:{}", Escape(victim->GetName()),
                                           victim->GetLevel(), pct));
    }

    if (status == RPG_GATHER)
    {
        if (auto const* gather = std::get_if<NewRpgInfo::Gather>(&rpg.data))
            rows.push_back(Acore::StringFormat("route:{}:{}:{}", gather->routeIndex,
                                               gather->nodesVisited, gather->zoneId));
    }

    if (status == RPG_FISH)
    {
        if (auto const* fish = std::get_if<NewRpgInfo::Fish>(&rpg.data))
            rows.push_back(Acore::StringFormat("fish:{}", fish->casts));
    }

    rows.push_back(Acore::StringFormat("zone:{}", to->GetZoneId()));
    rows.push_back(Acore::StringFormat("gold:{}", to->GetMoney() / GOLD));

    // Mirrored to the log. The panel shows this to whoever is playing, but when a character parks
    // and the answer is "which of these fields is wrong", reading it after the fact beats asking
    // someone to read numbers off their screen. Self bots only, so this is one line per request per
    // played character.
    {
        std::string joined;
        for (std::string const& row : rows)
        {
            if (!joined.empty())
                joined += " ";
            joined += row;
        }
        LOG_DEBUG("playerbots", "[SelfStat] {}", joined);
    }

    Reply(to, "SELFSTAT", "0", rows);
}

void BotInspectorMgr::HandleSelfLog(Player* to, uint32 afterSeq)
{
    if (!to)
        return;

    // Capped per reply rather than per poll. A client arriving late asks from sequence 0 and would
    // otherwise pull the whole ring in one message; it simply asks again with a higher sequence and
    // walks forward a page at a time.
    constexpr uint32 MAX_ROWS_PER_REPLY = 60;

    std::vector<std::string> rows = sBotEventLogMgr.GetSince(to, afterSeq, MAX_ROWS_PER_REPLY);

    // The key carries the sequence the client asked from, so a reply that crosses with a newer
    // request can be recognised as stale rather than appended twice.
    Reply(to, "SELFLOG", std::to_string(afterSeq), rows);
}

bool BotInspectorMgr::JoinMasterParty(Player* master, Player* bot)
{
    if (!master || !bot || master == bot)
        return false;

    // Not across factions.
    //
    // The client cannot form such a group and neither could anybody in 3.3.5 -- cross-faction
    // grouping arrived many expansions later. But this adds a member to a Group directly rather
    // than going through an invite, so nothing was ever asked, and picking alts from the panel
    // quietly put a blood elf in a party of five humans. It then sat there as a stale raid on a
    // different continent, costing her the group experience rate for kills nobody else was near.
    if (master->GetTeamId() != bot->GetTeamId())
        return false;

    if (Group* existing = bot->GetGroup())
    {
        // Already where it was asked to be.
        if (existing == master->GetGroup())
            return true;

        existing->RemoveMember(bot->GetGUID());
    }

    Group* group = master->GetGroup();
    if (!group)
    {
        group = new Group();
        if (!group->Create(master))
        {
            delete group;
            return false;
        }

        sGroupMgr->AddGroup(group);
    }

    if (group->IsFull())
        return false;

    return group->AddMember(bot);
}

void BotInspectorMgr::HandleAltControl(Player* to, std::string const& action, ObjectGuid::LowType altGuid)
{
    if (!to || !to->GetSession())
        return;

    ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(altGuid);

    // Ownership is enforced here and deliberately not left to the bot command layer. That layer
    // only checks the account for "addaccount"; a plain "add" trusts its caller, so driving it
    // straight from an addon message would otherwise let a crafted packet log in and puppet any
    // character on the realm. This is the check that makes the feature safe to expose.
    uint32 const owner = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    if (!owner || owner != to->GetSession()->GetAccountId())
    {
        SendError(to, 403, "not your character");
        return;
    }

    // Owning both characters does not make them groupable. Cross-faction grouping did not exist in
    // 3.3.5 and the client cannot produce it, but this panel adds members to a Group directly, so
    // the question had never been asked and picking alts from the list happily put a blood elf in
    // a party of five humans.
    //
    // Refused here rather than only inside JoinMasterParty, because that failure path queues a
    // pending invite and answers "pending" -- so the panel would have shown a request that was
    // never going to complete, which is a worse answer than no.
    if (CharacterCacheEntry const* cached = sCharacterCache->GetCharacterCacheByGuid(guid))
    {
        if (Player::TeamIdForRace(cached->Race) != to->GetTeamId())
        {
            SendError(to, 409, "that character is on the other faction");
            return;
        }
    }

    PlayerbotMgr* const mgr = GET_PLAYERBOT_MGR(to);
    if (!mgr)
    {
        SendError(to, 500, "no bot manager for this session");
        return;
    }

    Player* online = ObjectAccessor::FindPlayer(guid);

    if (action == "REMOVE")
    {
        if (!online)
        {
            SendError(to, 409, "that alt is not logged in");
            return;
        }

        // Drop any queued invite first, or the bot gets re-invited moments after being dismissed.
        _pendingInvites.erase(std::remove_if(_pendingInvites.begin(), _pendingInvites.end(),
                                             [&guid](PendingInvite const& p) { return p.bot == guid; }),
                              _pendingInvites.end());

        std::string const outcome =
            mgr->ProcessBotCommand("remove", guid, to->GetGUID(), true, to->GetSession()->GetAccountId(), 0);

        Reply(to, "ALTCTL", std::to_string(altGuid), {Acore::StringFormat("{}:{}", altGuid, Escape(outcome))});
        return;
    }

    if (action == "ROLE_TANK" || action == "ROLE_HEAL" || action == "ROLE_DPS")
    {
        if (!online)
        {
            SendError(to, 409, "that alt is not logged in");
            return;
        }

        PlayerbotAI* const ai = GET_PLAYERBOT_AI(online);
        if (!ai)
        {
            SendError(to, 409, "that character is being played");
            return;
        }

        RoleStrategies const& wanted = action == "ROLE_TANK" ? ROLE_TANK
                                     : action == "ROLE_HEAL" ? ROLE_HEAL
                                                             : ROLE_DPS;

        ai->ChangeStrategy(wanted.combat, BotState::BOT_STATE_COMBAT);
        ai->ChangeStrategy(wanted.nonCombat, BotState::BOT_STATE_NON_COMBAT);

        LOG_DEBUG("playerbots", "[Inspector] {} set {} to {}", to->GetName(), online->GetName(),
                  DescribeRole(ai));

        Reply(to, "ALTCTL", std::to_string(altGuid),
              {Acore::StringFormat("{}:{}", altGuid, DescribeRole(ai))});
        return;
    }

    if (action != "ADD" && action != "INVITE")
    {
        SendError(to, 400, "unknown alt action");
        return;
    }

    // Party capacity is checked before the login is started, so a refusal costs nothing and the
    // player is not left with a bot they did not want online.
    if (Group const* group = to->GetGroup())
    {
        uint32 alts = 0;
        for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member != to && GET_PLAYERBOT_AI(member))
                    ++alts;

        bool const alreadyIn = group->IsMember(guid);
        if (!alreadyIn && alts >= MAX_PARTY_ALTS)
        {
            SendError(to, 409, "party already has four alts");
            return;
        }
    }

    if (!online)
    {
        if (action == "INVITE")
        {
            SendError(to, 409, "that alt is not logged in");
            return;
        }

        std::string const outcome =
            mgr->ProcessBotCommand("add", guid, to->GetGUID(), true, to->GetSession()->GetAccountId(), 0);

        if (outcome != "ok")
        {
            Reply(to, "ALTCTL", std::to_string(altGuid), {Acore::StringFormat("{}:{}", altGuid, Escape(outcome))});
            return;
        }
    }
    else if (!GET_PLAYERBOT_AI(online))
    {
        // A human is playing this character right now. Taking it over is not ours to do.
        SendError(to, 409, "that character is being played");
        return;
    }

    // The bot may already be in world (INVITE, or an ADD that resolved immediately), in which case
    // this succeeds now and no entry is queued.
    if (online && JoinMasterParty(to, online))
    {
        Reply(to, "ALTCTL", std::to_string(altGuid), {Acore::StringFormat("{}:ok", altGuid)});
        return;
    }

    _pendingInvites.erase(std::remove_if(_pendingInvites.begin(), _pendingInvites.end(),
                                         [&guid](PendingInvite const& p) { return p.bot == guid; }),
                          _pendingInvites.end());
    _pendingInvites.push_back({to->GetGUID(), guid, INVITE_WAIT_MS});

    Reply(to, "ALTCTL", std::to_string(altGuid), {Acore::StringFormat("{}:pending", altGuid)});
}

void BotInspectorMgr::Update(Player* player, uint32 diff)
{
    if (!player || _pendingInvites.empty())
        return;

    for (auto itr = _pendingInvites.begin(); itr != _pendingInvites.end();)
    {
        // Each entry is driven by its own master's tick, so one player's pending invite is never
        // aged or retried by another player's update.
        if (itr->master != player->GetGUID())
        {
            ++itr;
            continue;
        }

        Player* const bot = ObjectAccessor::FindPlayer(itr->bot);
        if (bot && bot->IsInWorld() && JoinMasterParty(player, bot))
        {
            itr = _pendingInvites.erase(itr);
            continue;
        }

        if (itr->remainingMs <= diff)
        {
            LOG_DEBUG("playerbots", "[Inspector] gave up joining {} to {}'s party", itr->bot.ToString(),
                      player->GetName());
            itr = _pendingInvites.erase(itr);
            continue;
        }

        itr->remainingMs -= diff;
        ++itr;
    }
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
    else if (verb == "ALTS")
        HandleAlts(sender);
    else if (verb == "SELFSTAT")
        HandleSelfStat(sender);
    else if (verb == "SELFLOG")
        HandleSelfLog(sender, parts.size() >= 4
                                  ? static_cast<uint32>(std::strtoul(parts[3].c_str(), nullptr, 10))
                                  : 0u);
    else if (verb == "ALTCTL" && parts.size() >= 5)
        HandleAltControl(sender, parts[3],
                         static_cast<ObjectGuid::LowType>(std::strtoul(parts[4].c_str(), nullptr, 10)));
    else
        SendError(sender, 400, "unknown verb");

    return true;
}
