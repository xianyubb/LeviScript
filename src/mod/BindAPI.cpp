
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

void BindAPI(UniqueEnginePtr& engine) { engine->set("log", Function::newFunction(&Log)); }