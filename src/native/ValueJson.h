#pragma once

#include <nlohmann/json.hpp>

#include "script/Types.h"

namespace ls::script {
class ScriptEngine;
} // namespace ls::script

namespace ls::native {

/// Deep-convert a script value into JSON (used to pass data between separate
/// engines for cross-plugin RPC, and by tooling). Functions and native objects
/// are represented by a `{"$kind": ...}` marker because they cannot cross engines.
[[nodiscard]] nlohmann::json valueToJson(ls::script::ScriptEngine& engine, ls::script::ValueHandle handle, int depth = 8);

/// Rebuild a script value (owning handle) owned by `engine` from JSON.
[[nodiscard]] ls::script::ValueHandle jsonToValue(ls::script::ScriptEngine& engine, nlohmann::json const& value);

} // namespace ls::native
