#include "backend/quickjs/QuickJsException.h"

#include <string>

namespace ls::backend::quickjs {

namespace {

[[nodiscard]] std::string valueToString(JSContext* ctx, JSValueConst value) {
    if (JS_IsUndefined(value)) {
        return "undefined";
    }
    if (JS_IsNull(value)) {
        return "null";
    }
    size_t      len = 0;
    char const* str = JS_ToCStringLen(ctx, &len, value);
    if (str == nullptr) {
        JS_FreeValue(ctx, JS_GetException(ctx)); // drop the secondary conversion error
        return "<unprintable value>";
    }
    std::string result(str, len);
    JS_FreeCString(ctx, str);
    return result;
}

} // namespace

std::string formatExceptionValue(JSContext* ctx, JSValueConst exception) {
    std::string message = valueToString(ctx, exception);
    if (!JS_IsObject(exception) || JS_IsNull(exception)) {
        return message;
    }
    JSValue stackValue = JS_GetPropertyStr(ctx, exception, "stack");
    if (!JS_IsUndefined(stackValue) && !JS_IsNull(stackValue)) {
        std::string stack = valueToString(ctx, stackValue);
        JS_FreeValue(ctx, stackValue);
        if (!stack.empty()) {
            return message + "\n" + stack;
        }
        return message;
    }
    JS_FreeValue(ctx, stackValue);
    return message;
}

std::string consumeException(JSContext* ctx) {
    JSValue     exception = JS_GetException(ctx);
    std::string result    = formatExceptionValue(ctx, exception);
    JS_FreeValue(ctx, exception);
    return result;
}

JSValue throwJsError(JSContext* ctx, std::string_view message) {
    std::string tmp(message);
    return JS_ThrowInternalError(ctx, "%s", tmp.c_str());
}

JSValue throwJsTypeError(JSContext* ctx, std::string_view message) {
    std::string tmp(message);
    return JS_ThrowTypeError(ctx, "%s", tmp.c_str());
}

JSValue throwJsRangeError(JSContext* ctx, std::string_view message) {
    std::string tmp(message);
    return JS_ThrowRangeError(ctx, "%s", tmp.c_str());
}

} // namespace ls::backend::quickjs
