/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTrainingMgr.h"

#include "BudgetValues.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "QueryResult.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World.h"
#include "StringFormat.h"

#include <algorithm>
#include <array>
#include <vector>

namespace
{
/// How often a bot considers training. Frequent enough that a new rank is bought within a minute of
/// becoming available, rare enough that the query-free pass costs nothing at 500 bots.
constexpr uint32 TRAIN_INTERVAL_MS = 30 * 1000;
}  // namespace

void BotTrainingMgr::EnsureLoaded()
{
    std::call_once(_loadOnce, [this]()
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT SpellID, MoneyCost, ReqSkillLine, ReqSkillRank, ReqLevel, ReqSpell FROM npc_trainer");

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();

                // A negative SpellID means "include trainer template N", not a spell.
                int32 const spellId = fields[0].Get<int32>();
                if (spellId <= 0)
                    continue;

                TrainableSpell entry;
                entry.spellId = uint32(spellId);
                entry.cost = fields[1].Get<uint32>();
                entry.reqSkillLine = fields[2].Get<uint32>();
                entry.reqSkillRank = fields[3].Get<uint32>();
                entry.reqLevel = fields[4].Get<uint32>();
                entry.reqSpell = fields[5].Get<uint32>();

                _spells.push_back(entry);
            } while (result->NextRow());
        }

        // Highest requirement first, cheapest within a rank: a bot should buy the rank it has just
        // become eligible for, not whatever the table happened to list first.
        std::sort(_spells.begin(), _spells.end(), [](TrainableSpell const& a, TrainableSpell const& b)
        {
            if (a.reqLevel != b.reqLevel)
                return a.reqLevel > b.reqLevel;
            return a.cost < b.cost;
        });

        LOG_INFO("server.loading", ">> Loaded {} trainable spells for bot training", _spells.size());
    });
}

bool BotTrainingMgr::MaySpend(Player* bot)
{
    // Any bot spends its own gold on its own spells.
    //
    // This used to refuse alt bots on the grounds that a bot under a human's command should not
    // spend its owner's gold unasked. That reasoning does not survive contact with what an alt bot
    // is: the gold is in the alt's own pocket, and an alt that will not train is an alt that stays
    // useless until its owner drives it to a trainer by hand -- which is the chore the AI exists to
    // remove. The owner decides how much gold the alt carries; that is the real control.
    return GET_PLAYERBOT_AI(bot) != nullptr;
}

uint32 BotTrainingMgr::TaughtSpell(uint32 spellId)
{
    // npc_trainer's SpellID is sometimes the spell the trainer *casts*, whose effect teaches the
    // real one. Asking HasSpell about the wrapper then answers "no" forever, however many times it
    // is bought.
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return spellId;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (info->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL && info->Effects[i].TriggerSpell)
            return info->Effects[i].TriggerSpell;

    return spellId;
}

namespace
{
constexpr std::array<uint32, 11> PRIMARY_SKILLS = {
    SKILL_BLACKSMITHING, SKILL_LEATHERWORKING, SKILL_ALCHEMY, SKILL_HERBALISM, SKILL_MINING,
    SKILL_TAILORING, SKILL_ENGINEERING, SKILL_ENCHANTING, SKILL_SKINNING, SKILL_JEWELCRAFTING,
    SKILL_INSCRIPTION};

std::vector<uint32> HeldProfessions(Player* bot)
{
    std::vector<uint32> held;
    for (uint32 skill : PRIMARY_SKILLS)
        if (bot->HasSkill(skill))
            held.push_back(skill);

    return held;
}
}  // namespace

uint32 BotTrainingMgr::PrimaryProfessionTaught(uint32 spellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return 0;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (info->Effects[i].Effect == SPELL_EFFECT_SKILL && IsPrimaryProfessionSkill(info->Effects[i].MiscValue))
            return info->Effects[i].MiscValue;

    return 0;
}

std::pair<uint16, uint16> BotTrainingMgr::PreferredProfessions(Player* bot)
{
    // The factory already knows what each class should take -- paladins mining and blacksmithing,
    // mages tailoring and enchanting, and so on -- so that table is reused rather than a second
    // opinion invented next to it.
    std::vector<PlayerbotFactory::WeightedProfessionPair> const pairs = PlayerbotFactory::GetClassProfessionPairs(bot);
    if (pairs.empty())
        return {0, 0};

    uint32 total = 0;
    for (auto const& pair : pairs)
        total += pair.weight;

    if (!total)
        return {pairs[0].firstSkill, pairs[0].secondSkill};

    // Chosen from the guid rather than at random: this is asked on every training pass and must give
    // the same answer every time, or a bot would drift toward learning one of each.
    uint32 roll = uint32((bot->GetGUID().GetCounter() * 2654435761u) % total);
    for (auto const& pair : pairs)
    {
        if (roll < pair.weight)
            return {pair.firstSkill, pair.secondSkill};
        roll -= pair.weight;
    }

    return {pairs[0].firstSkill, pairs[0].secondSkill};
}

bool BotTrainingMgr::Qualifies(Player* bot, TrainableSpell const& entry)
{
    // Test knowledge of what is actually taught, not of the wrapper that teaches it.
    if (bot->HasSpell(TaughtSpell(entry.spellId)) || bot->HasSpell(entry.spellId))
        return false;

    if (entry.reqLevel && bot->GetLevel() < entry.reqLevel)
        return false;

    if (entry.reqSpell && !bot->HasSpell(entry.reqSpell))
        return false;

    if (entry.reqSkillLine && bot->GetBaseSkillValue(entry.reqSkillLine) < entry.reqSkillRank)
        return false;

    // Primary professions are capped at two, and which two is not arbitrary.
    //
    // Nothing here enforced either rule, so a bot with gold learned every profession in the game --
    // 123 bots ended up over the limit, one with eleven. The core tracks the allowance; a real
    // trainer refuses past it and so must this.
    //
    // Asked of the taught spell as well as the trainer's wrapper. Looking only at the wrapper let
    // a paladin who already had blacksmithing and mining go on to learn herbalism and alchemy: the
    // wrapper carries no SPELL_EFFECT_SKILL, so the profession went undetected and the gate never
    // ran at all.
    uint32 profession = PrimaryProfessionTaught(entry.spellId);
    if (!profession)
        profession = PrimaryProfessionTaught(TaughtSpell(entry.spellId));

    if (profession && !bot->HasSkill(profession))
    {
        // Counted from the skills the bot actually holds rather than read from
        // GetFreePrimaryProfessionPoints. That counter is maintained by add/remove of spells, and a
        // realm where professions have been edited underneath it -- as they have been here -- can
        // leave it disagreeing with the character sheet. The skills are the truth.
        if (HeldProfessions(bot).size() >= sWorld->getIntConfig(CONFIG_MAX_PRIMARY_TRADE_SKILL))
            return false;

        auto const [first, second] = PreferredProfessions(bot);
        if (profession != first && profession != second)
            return false;
    }

    // Only what this class or race can actually use. Without this a bot would learn every trainable
    // spell in the game, which is both nonsense and a way to make one bot capable of everything.
    //
    // Tested on the spell actually taught as well as the trainer's wrapper. A wrapper carries no skill
    // line, so the core's test passes it for everyone -- that is how a warrior reaching level 20 bought
    // the warlock's Felsteed, and how 367 characters of every class ended up holding it.
    if (!bot->IsSpellFitByClassAndRace(entry.spellId) ||
        !bot->IsSpellFitByClassAndRace(TaughtSpell(entry.spellId)))
        return false;

    return sSpellMgr->GetSpellInfo(entry.spellId) != nullptr;
}

void BotTrainingMgr::EnforceClassSpells(Player* bot)
{
    // Undo what the missing class check above let through: 1,554 spell rows across the realm, all of
    // them another class's trainer-taught mount or form -- Felsteed, Dreadsteed, both Warhorses, the
    // Thalassian Charger, Flight Form -- held by warriors, mages, rogues and everyone else.
    //
    // Only a *class* mismatch is removed. Race is deliberately left alone: a bot may hold another
    // race's mount legitimately (a reputation vendor sells them), and the core's own race test
    // would strip those too.
    std::vector<uint32> wrong;
    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED)
            continue;

        bool classRestricted = false;
        bool fitsClass = false;
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            if (!itr->second || !itr->second->ClassMask)
                continue;

            classRestricted = true;
            if (itr->second->ClassMask & bot->getClassMask())
                fitsClass = true;
        }

        if (classRestricted && !fitsClass)
            wrong.push_back(spellId);
    }

    for (uint32 spellId : wrong)
    {
        bot->removeSpell(spellId, SPEC_MASK_ALL, false);
        LOG_INFO("playerbots", "[Train] {} lost spell {}: it belongs to another class", bot->GetName(), spellId);
    }
}

void BotTrainingMgr::EnforceProfessions(Player* bot)
{
    auto const [want1, want2] = PreferredProfessions(bot);
    if (!want1 && !want2)
        return;

    for (uint32 skill : PRIMARY_SKILLS)
    {
        if (!bot->HasSkill(skill) || skill == want1 || skill == want2)
            continue;

        bot->SetSkill(skill, 0, 0, 0);

        // The apprentice spell goes with it, or the skill is simply recreated the next time spells
        // are loaded and the sweep runs forever against its own leftovers.
        if (uint32 const starter = PlayerbotFactory::GetProfessionStarterSpell(uint16(skill)))
            if (bot->HasSpell(starter))
                bot->removeSpell(starter, SPEC_MASK_ALL, false);

        std::unique_lock<std::shared_mutex> guard(_mutex);
        ++_professionStripped;

        LOG_DEBUG("playerbots", "[Professions] {} stripped of {}, which its class should not have",
                  bot->GetName(), skill);
    }
}

uint32 BotTrainingMgr::TrainNow(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !sPlayerbotAIConfig.remoteTrainingEnabled)
        return 0;

    if (!MaySpend(bot))
        return 0;

    EnsureLoaded();

    // Spend from the budget, not from the purse.
    //
    // "free money for spells" is what is left after everything ranked above spells has been set
    // aside -- guild dues, repairs, ammo. Reading GetMoney() instead meant a bot could train away
    // the gold it was holding for a repair, which is exactly the ordering the budget exists to
    // enforce and which I claimed was being honoured while this spent straight from the pocket.
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->GetAiObjectContext())
        return 0;

    // Clamped rather than trusted: the budget is derived from the purse, so it should never exceed
    // it, but spending more than the bot holds would be a far worse failure than training less.
    uint32 const budget = std::min(bot->GetMoney(),
                                   botAI->GetAiObjectContext()
                                       ->GetValue<uint32>("free money for",
                                                          std::to_string(uint32(NeedMoneyFor::spells)))
                                       ->Get());

    if (!budget)
        return 0;

    uint32 learned = 0;
    uint32 spent = 0;

    for (TrainableSpell const& entry : _spells)
    {
        if (learned >= sPlayerbotAIConfig.remoteTrainingMaxPerPass)
            break;

        if (!Qualifies(bot, entry))
            continue;

        bool repeatPurchase = false;
        {
            std::shared_lock<std::shared_mutex> guard(_mutex);

            if (_neverPersists.count(entry.spellId))
                continue;

            auto itr = _unteachable.find(bot->GetGUID());
            if (itr != _unteachable.end() && itr->second.count(entry.spellId))
                continue;

            auto bought = _purchased.find(bot->GetGUID());
            repeatPurchase = bought != _purchased.end() && bought->second.count(entry.spellId);
        }

        // Buying the same spell twice means the first one did not stick, whatever HasSpell said at
        // the time. Stop for this bot, and for every other bot too: the spell is the problem, not
        // the buyer.
        if (repeatPurchase)
        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            _neverPersists.insert(entry.spellId);
            _unteachable[bot->GetGUID()].insert(entry.spellId);

            LOG_DEBUG("playerbots", "[Train] spell {} did not persist for {}; excluded realm-wide",
                      entry.spellId, bot->GetName());
            continue;
        }

        // continue, not break: the list runs highest rank first, so an entry that is out of reach is
        // followed by cheaper lower ranks that are not.
        if (spent + entry.cost > budget)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(entry.spellId);
        uint32 const taught = TaughtSpell(entry.spellId);

        // Snapshot before the purchase, so the check afterwards is about what this spell did rather
        // than about what the bot happened to have.
        std::vector<uint32> const before = HeldProfessions(bot);

        bot->ModifyMoney(-int32(entry.cost));

        // Same branch a real trainer purchase takes: a spell whose effect is to teach must be cast,
        // not added to the spellbook. Adding the wrapper directly is what made every pass buy the
        // same spells again.
        if (info && info->HasEffect(SPELL_EFFECT_LEARN_SPELL))
            bot->CastSpell(bot, entry.spellId, true);
        else
            bot->learnSpell(entry.spellId, false);

        // A profession may arrive without ever looking like one.
        //
        // Predicting from the spell has now failed twice: first because the wrapper carries no
        // SPELL_EFFECT_SKILL, then again for shapes the unwrapping still does not see. Every such
        // fix is a guess about spell data, and the evidence kept saying the guess was wrong --
        // bots gained professions at value 1 with no apprentice spell to show for it.
        //
        // So stop predicting and look. If the purchase left the bot holding a profession it should
        // not have, take it straight back off. This cannot be bypassed by any spell shape, because
        // it inspects the bot rather than the recipe.
        for (uint32 skill : HeldProfessions(bot))
        {
            if (std::find(before.begin(), before.end(), skill) != before.end())
                continue;

            auto const [want1, want2] = PreferredProfessions(bot);
            bool const wanted = (skill == want1 || skill == want2);
            bool const room = before.size() < sWorld->getIntConfig(CONFIG_MAX_PRIMARY_TRADE_SKILL);

            if (wanted && room)
                continue;

            bot->SetSkill(skill, 0, 0, 0);
            bot->ModifyMoney(int32(entry.cost));

            std::unique_lock<std::shared_mutex> guard(_mutex);
            _unteachable[bot->GetGUID()].insert(entry.spellId);
            ++_professionRefused;

            LOG_DEBUG("playerbots", "[Train] {} gained profession {} it should not have; removed and refunded",
                      bot->GetName(), skill);
        }

        // Verify, refund and blacklist. Charging for something that does not stick is a loop that
        // empties a bot's purse thirty seconds at a time, so no purchase is trusted to have worked
        // just because it was made. This is the guard that makes the class of bug survivable, not
        // merely the one instance of it that was found.
        if (!bot->HasSpell(taught) && !bot->HasSpell(entry.spellId))
        {
            bot->ModifyMoney(int32(entry.cost));

            std::unique_lock<std::shared_mutex> guard(_mutex);
            _unteachable[bot->GetGUID()].insert(entry.spellId);
            ++_refunded;

            LOG_DEBUG("playerbots", "[Train] {} could not learn spell {}; refunded {}c and will not retry",
                      bot->GetName(), entry.spellId, entry.cost);
            continue;
        }

        spent += entry.cost;
        ++learned;

        {
            std::unique_lock<std::shared_mutex> guard(_mutex);
            _purchased[bot->GetGUID()].insert(entry.spellId);
        }

        LOG_DEBUG("playerbots", "[Train] {} learned spell {} (rank req {}) for {}c", bot->GetName(), entry.spellId,
                  entry.reqLevel, entry.cost);
    }

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _learnedTotal += learned;
        ++_passesRun;
    }

    return learned;
}

uint32 BotTrainingMgr::PendingCost(Player* bot)
{
    if (!bot || !sPlayerbotAIConfig.remoteTrainingEnabled)
        return 0;

    EnsureLoaded();

    uint32 total = 0;
    for (TrainableSpell const& entry : _spells)
        if (Qualifies(bot, entry))
            total += entry.cost;

    return total;
}

void BotTrainingMgr::Update(Player* bot, uint32 diff)
{
    if (!bot || !sPlayerbotAIConfig.remoteTrainingEnabled)
        return;

    ObjectGuid const guid = bot->GetGUID();

    {
        std::unique_lock<std::shared_mutex> guard(_mutex);
        uint32& timer = _timers[guid];

        if (timer > diff)
        {
            timer -= diff;
            return;
        }

        timer = TRAIN_INTERVAL_MS;
    }

    // Before training, not after: a bot carrying a profession it should not have would otherwise
    // spend the pass buying recipes for it.
    EnforceProfessions(bot);
    EnforceClassSpells(bot);
    TrainNow(bot);
}

void BotTrainingMgr::Forget(ObjectGuid guid)
{
    std::unique_lock<std::shared_mutex> guard(_mutex);
    _timers.erase(guid);
}

std::string BotTrainingMgr::DescribeStats() const
{
    std::shared_lock<std::shared_mutex> guard(_mutex);
    return Acore::StringFormat("training: {} spells learned over {} passes, {} bots tracked", _learnedTotal, _passesRun,
                               _timers.size());
}
