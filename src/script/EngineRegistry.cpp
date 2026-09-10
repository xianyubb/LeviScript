#include "script/EngineRegistry.h"

#include <utility>

#include "script/ScriptEngine.h"

namespace ls::script {

EngineRegistry& EngineRegistry::getInstance() {
    // Intentionally leaked ("immortal singleton"): it is reached from other static
    // destructors at process exit (a ScriptPlugin is torn down by LeviLamina's
    // ModManagerRegistry singleton during DLL detach). Destroying it in the normal
    // static-destruction phase would leave those callers with a dangling registry
    // and crash, so it is never destroyed; the OS reclaims it at process exit.
    static EngineRegistry* instance = new EngineRegistry();
    return *instance;
}

void EngineRegistry::registerEngine(std::string const& name, ScriptEngine* engine) {
    std::lock_guard lock(mMutex);
    mEngines.insert_or_assign(name, engine);
}

void EngineRegistry::unregisterEngine(std::string const& name) {
    std::lock_guard lock(mMutex);
    mEngines.erase(name);
}

ScriptEngine* EngineRegistry::getEngine(std::string const& name) {
    std::lock_guard lock(mMutex);
    auto            it = mEngines.find(name);
    return it == mEngines.end() ? nullptr : it->second;
}

std::vector<std::string> EngineRegistry::listEngines() {
    std::lock_guard        lock(mMutex);
    std::vector<std::string> result;
    result.reserve(mEngines.size());
    for (auto const& [name, engine] : mEngines) {
        result.push_back(name);
    }
    return result;
}

bool EngineRegistry::exportFunc(std::string const& key, ScriptEngine* owner, ValueHandle func) {
    std::lock_guard lock(mMutex);
    auto            it = mExports.find(key);
    if (it != mExports.end()) {
        // Replace an existing export: release the old handle on its owner engine.
        if (it->second.owner != nullptr && it->second.func != kInvalidHandle) {
            it->second.owner->release(it->second.func);
        }
        it->second = ExportedFunc{owner, func};
        return true;
    }
    mExports.emplace(key, ExportedFunc{owner, func});
    return true;
}

bool EngineRegistry::hasExport(std::string const& key) {
    std::lock_guard lock(mMutex);
    return mExports.find(key) != mExports.end();
}

ExportedFunc EngineRegistry::getExport(std::string const& key) {
    std::lock_guard lock(mMutex);
    auto            it = mExports.find(key);
    return it == mExports.end() ? ExportedFunc{} : it->second;
}

std::vector<std::string> EngineRegistry::listExports() {
    std::lock_guard        lock(mMutex);
    std::vector<std::string> result;
    result.reserve(mExports.size());
    for (auto const& [key, value] : mExports) {
        result.push_back(key);
    }
    return result;
}

void EngineRegistry::removeExportsOf(ScriptEngine* owner) {
    std::lock_guard lock(mMutex);
    for (auto it = mExports.begin(); it != mExports.end();) {
        if (it->second.owner == owner) {
            if (owner != nullptr && it->second.func != kInvalidHandle) {
                owner->release(it->second.func);
            }
            it = mExports.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace ls::script
