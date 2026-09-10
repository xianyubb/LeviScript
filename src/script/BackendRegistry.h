#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ll::io {
class Logger;
} // namespace ll::io

namespace ls::script {

class ScriptEngine;

/// Creates a backend engine for one plugin.
using EngineFactory = std::function<std::shared_ptr<ScriptEngine>(
    std::string const&                    pluginName,
    std::filesystem::path const&          rootDir,
    std::shared_ptr<ll::io::Logger> const& logger)>;

/// Maps a plugin manifest `type` (e.g. "lse-quickjs") to the backend that can
/// run it. New language backends register themselves here, which is the single
/// extension point needed to add Node.js / Python / Lua support later.
class BackendRegistry {
public:
    static BackendRegistry& getInstance();

    bool registerBackend(std::string type, EngineFactory factory);
    bool hasBackend(std::string_view type) const;
    [[nodiscard]] EngineFactory const* find(std::string_view type) const;
    [[nodiscard]] std::vector<std::string> listBackends() const;

private:
    std::unordered_map<std::string, EngineFactory> mFactories;
};

/// Convenience helper: registers a backend during static initialization.
struct BackendRegistrar {
    BackendRegistrar(std::string type, EngineFactory factory) {
        BackendRegistry::getInstance().registerBackend(std::move(type), std::move(factory));
    }
};

} // namespace ls::script
