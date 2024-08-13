#pragma once

#include "utils/UsingScriptX.h"

#include <string>
#include <vector>


class EngineManager {
public:
    static UniqueEnginePtr newEngine(std::string& modName);
    static bool            registerEngine(UniqueEnginePtr& engine);
    static bool            unregisterEngine( UniqueEnginePtr& engine);
    static bool            isValid(UniqueEnginePtr& engine, bool onlyCheckLocal = false);

    static std::vector<ScriptEngine*> getLocalEngines();
    static std::vector<ScriptEngine*> getGlobalEngines();
    static UniqueEnginePtr            getEngine(const std::string& name, bool onlyLocalEngine = false);

    static std::string& getEngineType(ScriptEngine* engine);
};
