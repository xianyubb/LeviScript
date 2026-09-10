#include <string>
#include <utility>

#include "ll/api/io/LogLevel.h"
#include "native/NativeApi.h"
#include "script/Local.h"
#include "script/ScriptEngine.h"
#include "script/Types.h"

namespace ls::native {

using ls::script::getGlobal;
using ls::script::kInvalidHandle;
using ls::script::Local;
using ls::script::makeFunction;
using ls::script::makeObject;
using ls::script::NativeFunction;
using ls::script::Object;
using ls::script::ScriptEngine;
using ls::script::ValueHandle;

namespace {

/// Build a variadic logger function: it joins every argument with a space (like
/// console.log) and forwards the result to the plugin's LeviLamina logger.
[[nodiscard]] NativeFunction makeLogFunction(ll::io::LogLevel level) {
    return [level](ScriptEngine& engine, ValueHandle, ValueHandle const* argv, int argc) -> ValueHandle {
        std::string message;
        for (int i = 0; i < argc; ++i) {
            if (i != 0) {
                message += " ";
            }
            message += engine.toStdString(argv[i]);
        }
        engine.logger().log(level, std::move(message));
        return kInvalidHandle;
    };
}

} // namespace

void bindLoggerApi(ScriptEngine& engine) {
    Local<Object> global = getGlobal(engine);
    Local<Object> logger = makeObject(engine);

    auto addMethod = [&](char const* name, ll::io::LogLevel level) {
        logger.setProperty(name, makeFunction(engine, makeLogFunction(level)));
    };
    addMethod("trace", ll::io::LogLevel::Trace);
    addMethod("debug", ll::io::LogLevel::Debug);
    addMethod("info", ll::io::LogLevel::Info);
    addMethod("warn", ll::io::LogLevel::Warn);
    addMethod("error", ll::io::LogLevel::Error);
    addMethod("fatal", ll::io::LogLevel::Fatal);

    global.setProperty("logger", logger);

    // LSE-compatible global shortcuts.
    global.setProperty("log", makeFunction(engine, makeLogFunction(ll::io::LogLevel::Info)));
    global.setProperty("logDebug", makeFunction(engine, makeLogFunction(ll::io::LogLevel::Debug)));
    global.setProperty("colorLog", makeFunction(engine, makeLogFunction(ll::io::LogLevel::Info)));
}

} // namespace ls::native
