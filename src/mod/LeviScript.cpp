#include "mod/LeviScript.h"

#include <memory>
#include <utility>

#include "backend/quickjs/QuickJsBackend.h"
#include "ll/api/mod/ModManager.h"
#include "ll/api/mod/ModManagerRegistry.h"
#include "ll/api/mod/RegisterHelper.h"
#include "plugin/PluginManager.h"
#include "script/BackendRegistry.h"

namespace levi_script {

LeviScript& LeviScript::getInstance() {
    static LeviScript instance;
    return instance;
}

bool LeviScript::load() {
    auto& logger = getSelf().getLogger();
    logger.debug("LeviScript loading...");

    // Register every language backend this build provides. Adding a new backend
    // (Node.js / Python / Lua) only means calling its registerBackend() here.
    ls::backend::quickjs::registerBackend();

    auto& backends = ls::script::BackendRegistry::getInstance();
    for (auto const& type : backends.listBackends()) {
        auto manager = std::make_shared<ls::plugin::PluginManager>(type);
        if (ll::mod::ModManagerRegistry::getInstance().addManager(manager)) {
            mManagers.push_back(std::move(manager));
            logger.info("Registered script backend '{}' (manifest type: {})", "LeviScript", type);
        } else {
            logger.error("Failed to register the script plugin manager for type '{}'", type);
        }
    }
    return true;
}

bool LeviScript::enable() {
    getSelf().getLogger().debug("LeviScript enabled");
    return true;
}

bool LeviScript::disable() {
    getSelf().getLogger().debug("LeviScript disabled");
    return true;
}

bool LeviScript::unload() {
    auto& registry = ll::mod::ModManagerRegistry::getInstance();
    for (auto const& manager : mManagers) {
        (void)registry.eraseManager(manager->getType());
    }
    mManagers.clear();
    return true;
}

} // namespace levi_script

LL_REGISTER_MOD(levi_script::LeviScript, levi_script::LeviScript::getInstance());
