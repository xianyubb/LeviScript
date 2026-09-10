#include "native/ValueJson.h"

#include <string>

#include "script/ScriptEngine.h"

namespace ls::native {

using ls::script::ScriptEngine;
using ls::script::ValueHandle;
using ls::script::ValueKind;

nlohmann::json valueToJson(ScriptEngine& engine, ValueHandle handle, int depth) {
    switch (engine.kindOf(handle)) {
    case ValueKind::Undefined: return nullptr;
    case ValueKind::Null: return nullptr;
    case ValueKind::Boolean: return engine.toBoolean(handle);
    case ValueKind::Integer: return engine.toInteger(handle);
    case ValueKind::Number: return engine.toNumber(handle);
    case ValueKind::String: return engine.toStdString(handle);
    case ValueKind::Symbol: return nlohmann::json{{"$kind", "symbol"}};
    case ValueKind::Function: return nlohmann::json{{"$kind", "function"}};
    case ValueKind::NativeObject: return nlohmann::json{{"$kind", "native"}};
    case ValueKind::Array: {
        if (depth <= 0) {
            return nlohmann::json{{"$kind", "array(truncated)"}};
        }
        nlohmann::json result = nlohmann::json::array();
        size_t         length = engine.getLength(handle);
        for (size_t i = 0; i < length; ++i) {
            ValueHandle element = engine.getElement(handle, i);
            result.push_back(valueToJson(engine, element, depth - 1));
            engine.release(element);
        }
        return result;
    }
    case ValueKind::Object:
    default: {
        if (depth <= 0) {
            return nlohmann::json{{"$kind", "object(truncated)"}};
        }
        nlohmann::json result = nlohmann::json::object();
        for (auto const& key : engine.getPropertyNames(handle)) {
            ValueHandle property = engine.getProperty(handle, key);
            result[key]          = valueToJson(engine, property, depth - 1);
            engine.release(property);
        }
        return result;
    }
    }
}

ValueHandle jsonToValue(ScriptEngine& engine, nlohmann::json const& value) {
    using nlohmann::json;
    switch (value.type()) {
    case json::value_t::null: return engine.newNull();
    case json::value_t::boolean: return engine.newBoolean(value.get<bool>());
    case json::value_t::number_integer:
    case json::value_t::number_unsigned: return engine.newInteger(value.get<int64_t>());
    case json::value_t::number_float: return engine.newNumber(value.get<double>());
    case json::value_t::string: return engine.newString(value.get_ref<json::string_t const&>());
    case json::value_t::array: {
        ValueHandle array = engine.newArray(value.size());
        size_t      index = 0;
        for (auto const& element : value) {
            ValueHandle item = jsonToValue(engine, element);
            engine.setElement(array, index++, item);
            engine.release(item);
        }
        return array;
    }
    case json::value_t::object: {
        ValueHandle object = engine.newObject();
        for (auto it = value.begin(); it != value.end(); ++it) {
            ValueHandle item = jsonToValue(engine, it.value());
            engine.setProperty(object, it.key(), item);
            engine.release(item);
        }
        return object;
    }
    default: return engine.newUndefined();
    }
}

} // namespace ls::native
