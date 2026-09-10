#pragma once

// EngineScope tracks the "current" engine on a per-thread basis.
//
// This is the backend-agnostic replacement for ScriptX's EngineScope. Native code
// that needs the engine of the currently running plugin (for example a timer
// callback fired from the server thread, or a Local<T> factory) relies on this
// thread local. Code invoked directly from a script should prefer the engine that
// is handed to the NativeFunction callback.

namespace ls::script {

class ScriptEngine;

namespace detail {
// `inline` guarantees a single shared instance across all translation units.
inline thread_local ScriptEngine* t_currentEngine = nullptr;
} // namespace detail

/// RAII guard that makes `engine` the current engine for the running thread.
class EngineScope {
    ScriptEngine* mPrevious;

public:
    explicit EngineScope(ScriptEngine* engine) : mPrevious(detail::t_currentEngine) {
        detail::t_currentEngine = engine;
    }
    ~EngineScope() { detail::t_currentEngine = mPrevious; }

    EngineScope(EngineScope const&)            = delete;
    EngineScope& operator=(EngineScope const&) = delete;
    EngineScope(EngineScope&&)                 = delete;
    EngineScope& operator=(EngineScope&&)      = delete;

    [[nodiscard]] static ScriptEngine* current() noexcept { return detail::t_currentEngine; }
};

/// RAII guard that temporarily clears the current engine (used when calling back
/// into LeviLamina / host code that must not be attributed to a script plugin).
class ExitEngineScope {
    ScriptEngine* mPrevious;

public:
    ExitEngineScope() : mPrevious(detail::t_currentEngine) { detail::t_currentEngine = nullptr; }
    ~ExitEngineScope() { detail::t_currentEngine = mPrevious; }

    ExitEngineScope(ExitEngineScope const&)            = delete;
    ExitEngineScope& operator=(ExitEngineScope const&) = delete;
    ExitEngineScope(ExitEngineScope&&)                 = delete;
    ExitEngineScope& operator=(ExitEngineScope&&)      = delete;
};

} // namespace ls::script
