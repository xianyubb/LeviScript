#pragma once

#include <quickjs.h>

#include <string>
#include <string_view>

// QuickJS <-> C++ exception helpers for the QuickJS backend.
//
// Native callbacks run inside QuickJS C frames, so a C++ exception must never
// unwind through the engine. Every boundary catches C++ exceptions and turns them
// into a JS exception; every failing JS call converts the pending JS exception
// into a ScriptException that C++ can reason about.

namespace ls::backend::quickjs {

/// Consume (and clear) the pending JS exception of `ctx`, formatted with stack.
[[nodiscard]] std::string consumeException(JSContext* ctx);

/// Format an already-retrieved exception value without consuming a new one.
[[nodiscard]] std::string formatExceptionValue(JSContext* ctx, JSValueConst exception);

[[nodiscard]] JSValue throwJsError(JSContext* ctx, std::string_view message);
[[nodiscard]] JSValue throwJsTypeError(JSContext* ctx, std::string_view message);
[[nodiscard]] JSValue throwJsRangeError(JSContext* ctx, std::string_view message);

} // namespace ls::backend::quickjs
