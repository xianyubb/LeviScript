#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "native/NativeApi.h"
#include "script/Local.h"
#include "script/ScriptEngine.h"
#include "script/Types.h"

namespace ls::native {

using ls::script::getGlobal;
using ls::script::Local;
using ls::script::Object;
using ls::script::ScriptEngine;
using ls::script::ValueHandle;
using ls::script::ValueKind;

void bindApis(ScriptEngine& engine) {
    bindPointerApi(engine); // first: other APIs may marshal raw pointers as NativePointer
    bindLoggerApi(engine);
    bindSystemApi(engine);
    bindLlApi(engine);
    // Generated from real headers (tools/autobind). Runs after bindLlApi so it can
    // merge into the existing `ll` namespace object.
    generated::bindGeneratedLlDataApi(engine); // ll/api/data/Version.h -> ll.Version
    bindMcApi(engine);
}

namespace {

[[nodiscard]] std::string kindName(ValueKind kind) {
    switch (kind) {
    case ValueKind::Undefined: return "undefined";
    case ValueKind::Null: return "null";
    case ValueKind::Boolean: return "boolean";
    case ValueKind::Integer: return "integer";
    case ValueKind::Number: return "number";
    case ValueKind::String: return "string";
    case ValueKind::Symbol: return "symbol";
    case ValueKind::Array: return "array";
    case ValueKind::Function: return "function";
    case ValueKind::NativeObject: return "native";
    case ValueKind::Object:
    default: return "object";
    }
}

/// Collect the method (function-valued) names declared on a prototype object.
[[nodiscard]] nlohmann::json listMethods(ScriptEngine& engine, ValueHandle prototype) {
    nlohmann::json methods = nlohmann::json::array();
    for (auto const& key : engine.getPropertyNames(prototype)) {
        if (key == "constructor") {
            continue;
        }
        ValueHandle member = engine.getProperty(prototype, key);
        if (engine.kindOf(member) == ValueKind::Function) {
            methods.push_back(key);
        }
        engine.release(member);
    }
    return methods;
}

/// Collect the static (function-valued) names declared on a constructor object.
[[nodiscard]] nlohmann::json listStatics(ScriptEngine& engine, ValueHandle constructor) {
    static std::vector<std::string> const skip = {"prototype", "length", "name", "caller", "arguments"};
    nlohmann::json                  statics = nlohmann::json::array();
    for (auto const& key : engine.getPropertyNames(constructor)) {
        if (std::find(skip.begin(), skip.end(), key) != skip.end()) {
            continue;
        }
        ValueHandle member = engine.getProperty(constructor, key);
        if (engine.kindOf(member) == ValueKind::Function) {
            statics.push_back(key);
        }
        engine.release(member);
    }
    return statics;
}

[[nodiscard]] nlohmann::json describe(ScriptEngine& engine, ValueHandle object, int depth) {
    nlohmann::json node = nlohmann::json::object();
    for (auto const& key : engine.getPropertyNames(object)) {
        ValueHandle    property = engine.getProperty(object, key);
        ValueKind      kind     = engine.kindOf(property);
        nlohmann::json entry;
        if (kind == ValueKind::Function) {
            // A constructor exposes a `prototype`; plain native functions do not.
            if (engine.hasProperty(property, "prototype")) {
                ValueHandle proto = engine.getProperty(property, "prototype");
                if (engine.kindOf(proto) == ValueKind::Object) {
                    entry["type"]          = "class";
                    entry["methods"]       = listMethods(engine, proto);
                    entry["staticMethods"] = listStatics(engine, property);
                } else {
                    entry["type"] = "function";
                }
                engine.release(proto);
            } else {
                entry["type"] = "function";
            }
        } else if (kind == ValueKind::Object && depth > 0) {
            entry["type"]    = "object";
            entry["members"] = describe(engine, property, depth - 1);
        } else {
            entry["type"] = kindName(kind);
        }
        node[key] = entry;
        engine.release(property);
    }
    return node;
}

} // namespace

std::string exportApiSurface(ScriptEngine& engine) {
    Local<Object> global = getGlobal(engine);
    nlohmann::json surface = describe(engine, global.handle(), 4);
    return surface.dump(2);
}

} // namespace ls::native
