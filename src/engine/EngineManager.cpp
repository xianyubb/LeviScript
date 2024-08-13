// 模仿 LSE 的一套新的引擎管理


#include "engine/EngineManager.h"
#include "engine/EngineOwnData.h"
#include "engine/GlobalShareData.h"
#include "utils/UsingScriptX.h"

// #include <map>
#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>


#include "ll/api/utils/StringUtils.h"


///////////////////////////////// API /////////////////////////////////

bool EngineManager::unregisterEngine(UniqueEnginePtr& engine) {
    std::unique_lock<std::shared_mutex> lock(globalShareData->engineVectorLock);
    globalShareData->globalEngineVector.erase(std::find_if(
        globalShareData->globalEngineVector.begin(),
        globalShareData->globalEngineVector.end(),
        [&](const ScriptEngine* _engine) { return _engine == engine.get(); }
    ));
    return false;
}

bool EngineManager::registerEngine(UniqueEnginePtr& engine) {
    std::unique_lock<std::shared_mutex> lock(globalShareData->engineVectorLock);
    globalShareData->globalEngineVector.push_back(engine.get());
    return true;
}

UniqueEnginePtr EngineManager::newEngine(std::string& modName) {
    std::unique_ptr<ScriptEngine, ScriptEngine::Deleter> engine = nullptr;

    // #if !defined(SCRIPTX_BACKEND_WEBASSEMBLY)
    engine = std::unique_ptr<ScriptEngine, ScriptEngine::Deleter>(new ScriptEngineImpl());
    // #else
    // engine = ScriptEngineImpl::instance();
    // #endif


    engine->setData(std::make_shared<EngineOwnData>());
    registerEngine(engine);
    if (!modName.empty()) {
        engine->getData<EngineOwnData>()->modName = modName;
    }
    return engine;
}

bool EngineManager::isValid(UniqueEnginePtr& engine, bool onlyCheckLocal) {
    std::shared_lock<std::shared_mutex> lock(globalShareData->engineVectorLock);
    for (auto i = globalShareData->globalEngineVector.begin(); i != globalShareData->globalEngineVector.end(); ++i)
        if (*i == engine.get()) {
            if (engine->isDestroying()) return false;
            if (onlyCheckLocal && getEngineType(engine.get()) != "JS") return false;
            else return true;
        }
    return false;
}

std::vector<ScriptEngine*> EngineManager::getLocalEngines() {
    std::vector<ScriptEngine*>          res;
    std::shared_lock<std::shared_mutex> lock(globalShareData->engineVectorLock);
    for (auto& engine : globalShareData->globalEngineVector) {
        if (getEngineType(engine) == "JS") res.push_back(engine);
    }
    return res;
}

std::vector<ScriptEngine*> EngineManager::getGlobalEngines() {
    std::vector<ScriptEngine*>          res;
    std::shared_lock<std::shared_mutex> lock(globalShareData->engineVectorLock);
    for (auto& engine : globalShareData->globalEngineVector) {
        res.push_back(engine);
    }
    return res;
}

UniqueEnginePtr EngineManager::getEngine(const std::string& name, bool onlyLocalEngine) {
    std::shared_lock<std::shared_mutex> lock(globalShareData->engineVectorLock);
    for (auto& _engine : globalShareData->globalEngineVector) {
        if (onlyLocalEngine && getEngineType(_engine) != "JS") continue;
        UniqueEnginePtr engine(_engine);
        auto            ownerData = engine.get()->getData<EngineOwnData>();
        if (ownerData == nullptr) continue;
        auto filename = ll::string_utils::u8str2str(
            std::filesystem::path(ll::string_utils::str2wstr(ownerData->modFileOrDirPath)).filename().u8string()
        );
        if (ownerData->modName == name || filename == name) {
            return UniqueEnginePtr(_engine);
        }
    }
    return nullptr;
}

std::string& EngineManager::getEngineType(ScriptEngine* engine) { return ENGINE_GET_DATA(engine)->engineType; }
