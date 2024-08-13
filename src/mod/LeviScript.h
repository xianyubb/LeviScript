#pragma once
#include "mod/ModManager.h"
#include "utils/UsingScriptX.h"

#include "ll/api/mod/NativeMod.h"


std::shared_ptr<ScriptEngine> createEngine();

namespace {
auto engine = createEngine();
};

namespace levi_script {

[[nodiscard]] auto getModManager() -> ls::ModManager&;
class LeviScript {

public:
    static LeviScript& getInstance();

    LeviScript(ll::mod::NativeMod& self) : mSelf(self) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }


    /// @return True if the mod is loaded successfully.
    bool load();

    /// @return True if the mod is enabled successfully.
    bool enable();

    /// @return True if the mod is disabled successfully.
    bool disable();

    // TODO: Implement this method if you need to unload the mod.
    // /// @return True if the mod is unloaded successfully.
    // bool unload();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace levi_script
