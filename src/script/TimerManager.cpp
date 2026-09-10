#include "script/TimerManager.h"

#include <exception>
#include <utility>

#include "ll/api/data/CancellableCallback.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "script/EngineScope.h"
#include "script/Exception.h"
#include "script/ScriptEngine.h"

namespace ls::script {

struct TimerManager::Timer {
    uint64_t                                      id        = 0;
    ValueHandle                                   func      = kInvalidHandle; // owning
    std::chrono::milliseconds                     delay{0};
    bool                                          repeat    = false;
    bool                                          cancelled = false;
    std::shared_ptr<ll::data::CancellableCallback> handle;
};

TimerManager::TimerManager(ScriptEngine* engine) : mEngine(engine) {}

TimerManager::~TimerManager() {
    // Runs while the owning engine is being destroyed: only cancel the pending
    // host callbacks so nothing fires afterwards. The function handles themselves
    // are freed en masse when the backend tears down its runtime, and calling the
    // (pure) virtual release() here would be undefined behaviour.
    for (auto& [id, timer] : mTimers) {
        timer->cancelled = true;
        if (timer->handle) {
            timer->handle->cancel();
        }
        timer->func = kInvalidHandle;
    }
    mTimers.clear();
}

void TimerManager::schedule(std::shared_ptr<Timer> const& timer) {
    uint64_t id    = timer->id;
    auto     delay = timer->delay.count() > 0 ? timer->delay : std::chrono::milliseconds(1);
    timer->handle  = ll::thread::ServerThreadExecutor::getDefault().executeAfter([this, id] { fire(id); }, delay);
}

void TimerManager::fire(uint64_t id) {
    auto it = mTimers.find(id);
    if (it == mTimers.end()) {
        return;
    }
    std::shared_ptr<Timer> timer = it->second;
    if (timer->cancelled) {
        mTimers.erase(it);
        return;
    }

    {
        EngineScope scope(mEngine);
        try {
            ValueHandle result = mEngine->call(timer->func, kInvalidHandle, nullptr, 0);
            if (result != kInvalidHandle) {
                mEngine->release(result);
            }
            mEngine->executePendingJobs();
        } catch (ScriptException const& e) {
            mEngine->logger().error("[Timer] Uncaught script exception: {}", e.fullMessage());
        } catch (std::exception const& e) {
            mEngine->logger().error("[Timer] Uncaught native exception: {}", e.what());
        } catch (...) {
            mEngine->logger().error("[Timer] Uncaught unknown exception");
        }
    }

    // The callback may have cleared (or even re-armed) this timer while running.
    it = mTimers.find(id);
    if (it == mTimers.end()) {
        return;
    }
    if (timer->repeat && !timer->cancelled) {
        schedule(timer);
    } else {
        if (timer->func != kInvalidHandle) {
            mEngine->release(timer->func);
            timer->func = kInvalidHandle;
        }
        mTimers.erase(id);
    }
}

uint64_t TimerManager::setTimeout(ValueHandle func, std::chrono::milliseconds delay) {
    if (func == kInvalidHandle || mEngine->kindOf(func) != ValueKind::Function) {
        if (func != kInvalidHandle) {
            mEngine->release(func);
        }
        return 0;
    }
    auto timer    = std::make_shared<Timer>();
    timer->id     = mNextId++;
    timer->func   = func;
    timer->delay  = delay;
    timer->repeat = false;
    mTimers.emplace(timer->id, timer);
    schedule(timer);
    return timer->id;
}

uint64_t TimerManager::setInterval(ValueHandle func, std::chrono::milliseconds interval) {
    if (func == kInvalidHandle || mEngine->kindOf(func) != ValueKind::Function) {
        if (func != kInvalidHandle) {
            mEngine->release(func);
        }
        return 0;
    }
    auto timer    = std::make_shared<Timer>();
    timer->id     = mNextId++;
    timer->func   = func;
    timer->delay  = interval;
    timer->repeat = true;
    mTimers.emplace(timer->id, timer);
    schedule(timer);
    return timer->id;
}

bool TimerManager::clear(uint64_t id) {
    auto it = mTimers.find(id);
    if (it == mTimers.end()) {
        return false;
    }
    std::shared_ptr<Timer> timer = it->second;
    timer->cancelled             = true;
    if (timer->handle) {
        timer->handle->cancel();
    }
    if (timer->func != kInvalidHandle) {
        mEngine->release(timer->func);
        timer->func = kInvalidHandle;
    }
    mTimers.erase(it);
    return true;
}

void TimerManager::clearAll() {
    for (auto& [id, timer] : mTimers) {
        timer->cancelled = true;
        if (timer->handle) {
            timer->handle->cancel();
        }
        if (timer->func != kInvalidHandle) {
            mEngine->release(timer->func);
            timer->func = kInvalidHandle;
        }
    }
    mTimers.clear();
}

} // namespace ls::script
