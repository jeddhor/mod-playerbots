/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChangeStrategyAction.h"
#include "Event.h"
#include "PlayerbotRepository.h"
#include "AiFactory.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"

// Helper function for prefixes used by combat and non-combat strategy commands.
static void HandleStrategyCommon(PlayerbotAI* botAI, std::string const& text, BotState state)
{
    std::vector<std::string> splitted = split(text, ',');
    for (std::vector<std::string>::iterator i = splitted.begin(); i != splitted.end(); i++)
    {
        const char* name = i->c_str();
        switch (name[0])
        {
            case '+':
            case '-':
            case '~':
                PlayerbotRepository::instance().Save(botAI);
                break;
            case '!':
                botAI->SelectiveResetStrategies(state);
                PlayerbotRepository::instance().Save(botAI);
                break;
            case '?':
                break;
        }
    }
}

bool ChangeCombatStrategyAction::Execute(Event event)
{
    std::string const text = event.getParam();

    int8 const roleBefore = AiFactory::GetSpecTabForAssignedRole(bot);

    botAI->ChangeStrategy(text.empty() ? getName() : text, BOT_STATE_COMBAT);
    if (event.GetSource() == "co")
        HandleStrategyCommon(botAI, text, BOT_STATE_COMBAT);

    // Giving a bot a role re-specs it into that role's tree.
    //
    // Without this the two can disagree, and the tree wins: GetPlayerSpecTab reads the talents as
    // soon as any point is spent, so a priest told to heal but specced Shadow is relabelled DPS by
    // the next ResetStrategies() -- which a group invite and an LFG join both trigger -- and stops
    // healing partway through a dungeon.
    //
    // Only fires when the *role* changed, not on any combat strategy edit, so tuning a bot's
    // rotation never costs it its build.
    if (int8 const roleAfter = AiFactory::GetSpecTabForAssignedRole(bot);
        roleAfter >= 0 && roleAfter != roleBefore)
    {
        PlayerbotFactory::RespecToAssignedRole(bot);
    }

    return true;
}

bool ChangeNonCombatStrategyAction::Execute(Event event)
{
    std::string const text = event.getParam();

    uint32 account = bot->GetSession()->GetAccountId();
    if (sPlayerbotAIConfig.IsInRandomAccountList(account) && botAI->GetMaster() &&
        !botAI->GetMaster()->CanBeGameMaster())
    {
        if (text.find("loot") != std::string::npos)
        {
            botAI->TellError("You can change any strategy except loot");
            return false;
        }
    }

    botAI->ChangeStrategy(text, BOT_STATE_NON_COMBAT);
    if (event.GetSource() == "nc")
        HandleStrategyCommon(botAI, text, BOT_STATE_NON_COMBAT);

    return true;
}

bool ChangeDeadStrategyAction::Execute(Event event)
{
    std::string const text = event.getParam();
    botAI->ChangeStrategy(text, BOT_STATE_DEAD);
    return true;
}
