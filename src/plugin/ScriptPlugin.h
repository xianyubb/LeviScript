#pragma once

#include <memory>
#include <string>

#include "ll/api/mod/Mod.h"

namespace ls::script {
class ScriptEngine;
} // namespace ls::script

namespace ls::plugin {

/// A loaded script plugin: an ll::mod::Mod that owns its script engine.
///
/// The engine is created and the entry script executed by the PluginManager
/// before the mod is registered; this class wires the LeviLamina mod lifecycle
/// (load/enable/disable/unload) to the optional script-side hooks
/// `onLoad` / `onEnable` / `onDisable` / `onUnload` and performs engine cleanup.
class ScriptPlugin final : public ll::mod::Mod {
public:
    ScriptPlugin(ll::mod::Manifest manifest, std::shared_ptr<ls::script::ScriptEngine> engine);
    // ll::mod::Mod has a non-virtual destructor; instances are always held through
    // a shared_ptr created as ScriptPlugin, so destruction stays correct.
    ~ScriptPlugin();

    [[nodiscard]] std::shared_ptr<ls::script::ScriptEngine> const& getEngine() const noexcept { return mEngine; }

    // Public wrappers around the protected ll::mod::Mod lifecycle invokers so the
    // PluginManager can drive them.
    ll::Expected<> doLoad();
    ll::Expected<> doUnload();
    ll::Expected<> doEnable();
    ll::Expected<> doDisable();

private:
    bool callHook(std::string const& name);
    void cleanupEngine();

    std::shared_ptr<ls::script::ScriptEngine> mEngine;
};

} // namespace ls::plugin
