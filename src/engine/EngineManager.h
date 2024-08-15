#pragma once

#include "utils/UsingScriptX.h"

#include <string>
#include <vector>


class EngineManager {
public:
    static ScriptEngine* newEngine(std::string& modName);
    static bool          registerEngine(ScriptEngine* engine);
    static bool          unregisterEngine(ScriptEngine* engine);
    static bool          isValid(ScriptEngine* engine, bool onlyCheckLocal = false);

    static std::vector<ScriptEngine*> getLocalEngines();
    static std::vector<ScriptEngine*> getGlobalEngines();
    static ScriptEngine*              getEngine(const std::string& name, bool onlyLocalEngine = false);

    static std::string& getEngineType(ScriptEngine* engine);
};
