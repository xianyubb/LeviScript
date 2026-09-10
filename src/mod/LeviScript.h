#pragma once

#include <memory>
#include <vector>

#include "ll/api/mod/NativeMod.h"

namespace ll::mod {
class ModManager;
} // namespace ll::mod

namespace levi_script {

/// The LeviScript mod: a script-plugin loader for LeviLamina.
///
/// On load it registers every available language backend (QuickJS today) and
/// creates one ll::mod::ModManager per backend type so that LeviLamina's own mod
/// system discovers, orders and routes script plugins to us - exactly the way
/// LegacyScriptEngine integrates, but without ScriptX and with a from-scratch
/// engine abstraction that supports native class inheritance.
class LeviScript {
public:
    static LeviScript& getInstance();

    LeviScript() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    ll::mod::NativeMod&                     mSelf;
    std::vector<std::shared_ptr<ll::mod::ModManager>> mManagers;
};

} // namespace levi_script
