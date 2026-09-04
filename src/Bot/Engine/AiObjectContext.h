/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AIOBJECTCONTEXT_H
#define PLAYERBOTS_AIOBJECTCONTEXT_H

#include "Common.h"
#include "DynamicObject.h"
#include "NamedObjectContext.h"
#include "PlayerbotAIAware.h"
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"
#include <sstream>
#include <string>

class PlayerbotAI;

typedef Strategy* (*StrategyCreator)(PlayerbotAI* botAI);
typedef Action* (*ActionCreator)(PlayerbotAI* botAI);
typedef Trigger* (*TriggerCreator)(PlayerbotAI* botAI);
typedef UntypedValue* (*ValueCreator)(PlayerbotAI* botAI);

class AiObjectContext : public PlayerbotAIAware
{
public:
    static BoolCalculatedValue* custom_glyphs(PlayerbotAI* ai); // Added for cutom glyphs
    AiObjectContext(PlayerbotAI* botAI,
                    SharedNamedObjectContextList<Strategy>& sharedStrategyContext = sharedStrategyContexts,
                    SharedNamedObjectContextList<Action>& sharedActionContext = sharedActionContexts,
                    SharedNamedObjectContextList<Trigger>& sharedTriggerContext = sharedTriggerContexts,
                    SharedNamedObjectContextList<UntypedValue>& sharedValueContext = sharedValueContexts);
    virtual ~AiObjectContext() {}

    virtual Strategy* GetStrategy(std::string const name);
    virtual std::set<std::string> GetSiblingStrategy(std::string const name);
    virtual Trigger* GetTrigger(std::string const name);
    virtual Action* GetAction(std::string const name);
    virtual UntypedValue* GetUntypedValue(std::string const& name);

    // These run on the hot path: AI_VALUE / AI_VALUE2 expand to GetValue and there are ~2500 call
    // sites, ~1500 of them the qualified (AI_VALUE2) form. Take the name by reference rather than
    // by value - the by-value parameters used to copy the string once per layer on the way down to
    // GetUntypedValue.
    template <class T>
    Value<T>* GetValue(std::string const& name)
    {
        return dynamic_cast<Value<T>*>(GetUntypedValue(name));
    }

    template <class T>
    Value<T>* GetValue(std::string const& name, std::string const& param)
    {
        // Build the "name::param" key in one buffer. The old `std::string(name) + "::" + param`
        // materialised three temporaries per call.
        std::string qualified;
        qualified.reserve(name.size() + 2 + param.size());
        qualified += name;
        qualified += "::";
        qualified += param;
        return GetValue<T>(qualified);
    }

    template <class T>
    Value<T>* GetValue(std::string const& name, int32 param)
    {
        // std::to_string, not an ostringstream. Constructing an ostringstream per call drags in
        // locale setup, a sentry and its own allocation - all to format one integer, ~1500 times
        // over per tick across the bot population.
        return GetValue<T>(name, std::to_string(param));
    }

    std::set<std::string> GetValues();
    std::set<std::string> GetSupportedStrategies();
    std::set<std::string> GetSupportedActions();
    std::string const FormatValues();

    std::vector<std::string> Save();
    void Load(std::vector<std::string> data);

    std::vector<std::string> performanceStack;

    static void BuildAllSharedContexts();

    static void BuildSharedContexts();
    static void BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts);
    static void BuildSharedActionContexts(SharedNamedObjectContextList<Action>& actionContexts);
    static void BuildSharedTriggerContexts(SharedNamedObjectContextList<Trigger>& triggerContexts);
    static void BuildSharedValueContexts(SharedNamedObjectContextList<UntypedValue>& valueContexts);

protected:
    NamedObjectContextList<Strategy> strategyContexts;
    NamedObjectContextList<Action> actionContexts;
    NamedObjectContextList<Trigger> triggerContexts;
    NamedObjectContextList<UntypedValue> valueContexts;

private:
    static SharedNamedObjectContextList<Strategy> sharedStrategyContexts;
    static SharedNamedObjectContextList<Action> sharedActionContexts;
    static SharedNamedObjectContextList<Trigger> sharedTriggerContexts;
    static SharedNamedObjectContextList<UntypedValue> sharedValueContexts;
};

#endif
