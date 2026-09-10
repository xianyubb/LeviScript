#include <chrono>
#include <cstdint>

#include "native/NativeApi.h"
#include "script/EngineScope.h"
#include "script/Local.h"
#include "script/ScriptEngine.h"
#include "script/TimerManager.h"
#include "script/bind/Bind.h"

namespace ls::native {

using ls::script::EngineScope;
using ls::script::getGlobal;
using ls::script::Local;
using ls::script::makeFunction;
using ls::script::makeNativeFunction;
using ls::script::Object;
using ls::script::ScriptEngine;
using ls::script::Value;

void bindSystemApi(ScriptEngine& engine) {
    Local<Object> global = getGlobal(engine);

    // setTimeout(callback, delayMs) -> timerId
    global.setProperty(
        "setTimeout",
        makeFunction(engine, makeNativeFunction([](Local<Value> callback, double delayMs) -> int64_t {
                         ScriptEngine& self = *callback.engine();
                         auto          ms   = std::chrono::milliseconds(static_cast<long long>(delayMs));
                         return static_cast<int64_t>(self.timers().setTimeout(callback.abandon(), ms));
                     }))
    );

    // setInterval(callback, intervalMs) -> timerId
    global.setProperty(
        "setInterval",
        makeFunction(engine, makeNativeFunction([](Local<Value> callback, double intervalMs) -> int64_t {
                         ScriptEngine& self = *callback.engine();
                         auto          ms   = std::chrono::milliseconds(static_cast<long long>(intervalMs));
                         return static_cast<int64_t>(self.timers().setInterval(callback.abandon(), ms));
                     }))
    );

    // clearTimeout(id) / clearInterval(id)
    auto clearFn = makeNativeFunction([](int64_t id) -> bool {
        ScriptEngine* self = EngineScope::current();
        return self != nullptr && self->timers().clear(static_cast<uint64_t>(id));
    });
    global.setProperty("clearTimeout", makeFunction(engine, clearFn));
    global.setProperty("clearInterval", makeFunction(engine, clearFn));

    // systemTimeMillis() -> wall clock milliseconds since epoch
    global.setProperty(
        "systemTimeMillis",
        makeFunction(engine, makeNativeFunction([]() -> int64_t {
                         return std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()
                         )
                             .count();
                     }))
    );
}

} // namespace ls::native
