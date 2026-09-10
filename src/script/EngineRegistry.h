#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/Types.h"

namespace ls::script {

class ScriptEngine;

/// One function exported by a plugin for cross-plugin calls. The handle is owned
/// by `owner` and stays valid until that engine is unregistered.
struct ExportedFunc {
    ScriptEngine* owner = nullptr;
    ValueHandle   func  = kInvalidHandle;
};

/// Process-wide registry of live plugin engines and their exported functions.
///
/// Used by the `ll` API for ll.listPlugins() and for the cross-plugin
/// exportFunc/importFunc RPC. All access happens on the server thread; the mutex
/// only guards against re-entrancy during (un)registration.
class EngineRegistry {
public:
    static EngineRegistry& getInstance();

    void           registerEngine(std::string const& name, ScriptEngine* engine);
    void           unregisterEngine(std::string const& name);
    ScriptEngine*  getEngine(std::string const& name);
    std::vector<std::string> listEngines();

    /// Adopt an owning `func` handle and expose it under `key`.
    bool         exportFunc(std::string const& key, ScriptEngine* owner, ValueHandle func);
    bool         hasExport(std::string const& key);
    ExportedFunc getExport(std::string const& key);
    std::vector<std::string> listExports();

    /// Release every export owned by `owner` (called before the engine dies).
    void removeExportsOf(ScriptEngine* owner);

private:
    std::mutex                                        mMutex;
    std::unordered_map<std::string, ScriptEngine*>    mEngines;
    std::unordered_map<std::string, ExportedFunc>     mExports;
};

} // namespace ls::script
