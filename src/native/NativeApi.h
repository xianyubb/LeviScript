#pragma once

#include <string>

namespace ls::script {
class ScriptEngine;
} // namespace ls::script

namespace ls::native {

// Each backend-agnostic API group binds itself into an engine's global object.
// They are declared here and defined in src/native/api/*.cpp so that new groups
// (or generated bindings) can be added without touching the orchestrator's
// translation unit more than once.

void bindLoggerApi(ls::script::ScriptEngine& engine); // logger + log/logDebug/colorLog
void bindSystemApi(ls::script::ScriptEngine& engine); // timers + time utilities
void bindPointerApi(ls::script::ScriptEngine& engine);// the universal NativePointer system
void bindLlApi(ls::script::ScriptEngine& engine);     // the `ll` namespace
void bindMcApi(ls::script::ScriptEngine& engine);     // sample `mc` namespace (inheritance demo)

namespace generated {
// Bindings produced from real LeviLamina/Minecraft headers by tools/autobind
// (sources live in src/native/generated/, one file per source header). Each merges
// into the matching hand-written namespace so the script API follows LL's layout.

/// Registrar emitted by export_ll_tree.py: calls every per-header binding function for
/// the exported ll (+ referenced mc) tree, ordered so base classes are registered first.
void bindAllGeneratedLl(ls::script::ScriptEngine& engine);
} // namespace generated

/// Bind every native API group into the engine (called once per plugin load).
void bindApis(ls::script::ScriptEngine& engine);

/// Serialize the engine's global API surface (namespaces, functions, classes and
/// their members) as a JSON string. Consumed by the API validator tool.
[[nodiscard]] std::string exportApiSurface(ls::script::ScriptEngine& engine);

} // namespace ls::native
