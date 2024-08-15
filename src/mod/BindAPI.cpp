#include "API/EventAPI/EventAPI.h"
#include "API/PlayerAPI/PlayerAPI.h"
#include "engine/EngineOwnData.h"
#include "utils/UsingScriptX.h"
#include <string>


Local<Value> Log(const std::string& str) {

    try {
        ENGINE_OWN_DATA()->logger.info(str);
        return Boolean::newBoolean(true);
    } catch (...) {}
    // CATCH("Fail in Log!");
    return Boolean::newBoolean(false);
}

void BindAPI(ScriptEngine* engine) {
    engine->set("log", Function::newFunction(&Log));
    engine->registerNativeClass(McEventClassBuilder);
    engine->registerNativeClass(PlayerClassBuilder);
}