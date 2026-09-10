#include "plugin/ScriptPlugin.h"

#include <exception>
#include <utility>

#include "ll/api/Expected.h"
#include "script/EngineRegistry.h"
#include "script/EngineScope.h"
#include "script/Exception.h"
#include "script/Local.h"
#include "script/ScriptEngine.h"
#include "script/TimerManager.h"

namespace ls::plugin {

ScriptPlugin::ScriptPlugin(ll::mod::Manifest manifest, std::shared_ptr<ls::script::ScriptEngine> engine)
: ll::mod::Mod(std::move(manifest)), mEngine(std::move(engine)) {
    onLoad([this](ll::mod::Mod&) { return callHook("onLoad"); });
    onEnable([this](ll::mod::Mod&) { return callHook("onEnable"); });
    onDisable([this](ll::mod::Mod&) { return callHook("onDisable"); });
    onUnload([this](ll::mod::Mod&) {
        bool result = callHook("onUnload");
        cleanupEngine();
        return result;
    });
}

ScriptPlugin::~ScriptPlugin() { cleanupEngine(); }

bool ScriptPlugin::callHook(std::string const& name) {
    if (!mEngine) {
        return true;
    }
    ls::script::EngineScope scope(mEngine.get());
    try {
        ls::script::Local<ls::script::Object> global = ls::script::getGlobal(*mEngine);
        ls::script::Local<ls::script::Value>  hook   = global.getProperty(name);
        if (hook.isFunction()) {
            (void)hook.call();
        }
        mEngine->executePendingJobs();
        return true;
    } catch (ls::script::ScriptException const& e) {
        mEngine->logger().error("Script hook '{}' threw: {}", name, e.fullMessage());
        return false;
    } catch (std::exception const& e) {
        mEngine->logger().error("Script hook '{}' native error: {}", name, e.what());
        return false;
    }
}

void ScriptPlugin::cleanupEngine() {
    if (!mEngine) {
        return;
    }
    ls::script::EngineScope scope(mEngine.get());
    mEngine->timers().clearAll();
    ls::script::EngineRegistry::getInstance().removeExportsOf(mEngine.get());
    ls::script::EngineRegistry::getInstance().unregisterEngine(mEngine->name());
}

ll::Expected<> ScriptPlugin::doLoad() { return onLoad(); }
ll::Expected<> ScriptPlugin::doUnload() { return onUnload(); }
ll::Expected<> ScriptPlugin::doEnable() { return onEnable(); }
ll::Expected<> ScriptPlugin::doDisable() { return onDisable(); }

} // namespace ls::plugin
