#include "plugin/PluginManager.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "baselib/BaseLib.h"
#include "ll/api/Expected.h"
#include "ll/api/io/FileUtils.h"
#include "ll/api/io/LoggerRegistry.h"
#include "ll/api/mod/Mod.h"
#include "native/NativeApi.h"
#include "plugin/ScriptPlugin.h"
#include "script/BackendRegistry.h"
#include "script/EngineRegistry.h"
#include "script/EngineScope.h"
#include "script/ScriptEngine.h"

namespace fs = std::filesystem;

namespace ls::plugin {

namespace {

/// Decide whether the plugin entry should run as an ES module. `.mjs` forces it,
/// `.cjs` forbids it, otherwise a `package.json` with `"type": "module"` opts in.
[[nodiscard]] bool detectEsModule(fs::path const& pluginDir, std::string const& entry) {
    fs::path entryPath(entry);
    if (entryPath.extension() == ".mjs") {
        return true;
    }
    if (entryPath.extension() == ".cjs") {
        return false;
    }
    std::error_code ec;
    fs::path        packageJson = pluginDir / "package.json";
    if (fs::exists(packageJson, ec)) {
        if (auto content = ll::file_utils::readFile(packageJson)) {
            try {
                auto json = nlohmann::json::parse(*content);
                if (json.contains("type") && json.at("type") == "module") {
                    return true;
                }
            } catch (std::exception const&) {
                // malformed package.json: fall back to classic script
            }
        }
    }
    return false;
}

} // namespace

PluginManager::PluginManager(std::string type) : ll::mod::ModManager(std::move(type)) {}

PluginManager::~PluginManager() = default;

ll::Expected<> PluginManager::load(ll::mod::Manifest manifest) {
    if (hasMod(manifest.name)) {
        return ll::makeStringError("Plugin '" + manifest.name + "' has already been loaded");
    }
    auto const* factory = ls::script::BackendRegistry::getInstance().find(manifest.type);
    if (factory == nullptr) {
        return ll::makeStringError("No script backend registered for type '" + manifest.type + "'");
    }

    fs::path pluginDir = ll::mod::getModsRoot() / manifest.name;
    auto     logger    = ll::io::LoggerRegistry::getInstance().getOrCreate(manifest.name);

    std::shared_ptr<ls::script::ScriptEngine> engine;
    try {
        engine = (*factory)(manifest.name, pluginDir, logger);
    } catch (std::exception const& e) {
        return ll::makeStringError("Failed to create engine for '" + manifest.name + "': " + e.what());
    }
    if (!engine) {
        return ll::makeStringError("Engine factory returned null for '" + manifest.name + "'");
    }

    ls::script::EngineScope scope(engine.get());

    ls::native::bindApis(*engine);

    if (auto result = engine->eval(ls::baselib::kBaseLibSource, "BaseLib.js"); !result) {
        return result;
    }

    ls::script::EngineRegistry::getInstance().registerEngine(manifest.name, engine.get());

    auto plugin = std::make_shared<ScriptPlugin>(manifest, engine);

    fs::path entryPath = pluginDir / manifest.entry;
    bool     asModule  = detectEsModule(pluginDir, manifest.entry);
    if (auto result = engine->loadFile(entryPath, asModule); !result) {
        ls::script::EngineRegistry::getInstance().unregisterEngine(manifest.name);
        return result;
    }

    if (auto result = plugin->doLoad(); !result) {
        ls::script::EngineRegistry::getInstance().unregisterEngine(manifest.name);
        return result;
    }

    addMod(manifest.name, plugin);
    return {};
}

ll::Expected<> PluginManager::unload(std::string_view name) {
    auto mod = getMod(name);
    if (!mod) {
        return ll::makeStringError("Plugin '" + std::string(name) + "' not found");
    }
    auto plugin = std::static_pointer_cast<ScriptPlugin>(mod);
    if (auto result = plugin->doUnload(); !result) {
        return result;
    }
    eraseMod(name);
    return {};
}

} // namespace ls::plugin
