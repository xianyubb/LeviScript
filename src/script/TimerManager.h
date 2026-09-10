#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "script/Types.h"

namespace ll::data {
class CancellableCallback;
} // namespace ll::data

namespace ls::script {

class ScriptEngine;

/// Backend-agnostic setTimeout / setInterval / clearTimeout / clearInterval.
///
/// Timers are scheduled through LeviLamina's ServerThreadExecutor so callbacks
/// always run on the server thread (the only thread allowed to touch an engine).
/// The stored function is an *owning* ValueHandle; it is released when the timer
/// is cleared or after a one-shot timer fires.
class TimerManager {
public:
    explicit TimerManager(ScriptEngine* engine);
    ~TimerManager();

    TimerManager(TimerManager const&)            = delete;
    TimerManager& operator=(TimerManager const&) = delete;

    /// Adopt an owning `func` handle and run it once after `delay`.
    /// Returns a non-zero timer id, or 0 when `func` is not callable.
    [[nodiscard]] uint64_t setTimeout(ValueHandle func, std::chrono::milliseconds delay);

    /// Adopt an owning `func` handle and run it every `interval`.
    [[nodiscard]] uint64_t setInterval(ValueHandle func, std::chrono::milliseconds interval);

    /// Cancel a pending timer and release its function handle.
    bool clear(uint64_t id);

    /// Cancel every pending timer (used on plugin unload while the engine lives).
    void clearAll();

private:
    struct Timer;

    void fire(uint64_t id);
    void schedule(std::shared_ptr<Timer> const& timer);

    ScriptEngine*                                      mEngine;
    std::unordered_map<uint64_t, std::shared_ptr<Timer>> mTimers;
    uint64_t                                           mNextId = 1;
};

} // namespace ls::script
