#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "script/ScriptEngine.h"
#include "script/Types.h"

namespace ls::script {

// ---------------------------------------------------------------------------
// Type tags. They carry no data; they only give Local<T> a static "shape" so
// that code reads like ScriptX (Local<Object>, Local<Function>, ...). Runtime
// type checks are performed through the engine.
// ---------------------------------------------------------------------------
struct Value {};
struct Undefined : Value {};
struct Null : Value {};
struct Boolean : Value {};
struct Number : Value {};
struct Integer : Number {};
struct String : Value {};
struct Symbol : Value {};
struct Object : Value {};
struct Array : Object {};
struct Function : Object {};
struct NativeObject : Object {};

/// RAII wrapper around an *owning* ValueHandle.
///
/// Copying retains (creates a second owning handle), moving transfers ownership,
/// destruction releases. Different tags are inter-convertible because the real
/// type is only known at runtime; use the is*() helpers to check.
template <typename Tag>
class Local {
public:
    using tag = Tag;

    Local() noexcept : mEngine(nullptr), mHandle(kInvalidHandle) {}

    /// Adopt an owning handle.
    Local(ScriptEngine* engine, ValueHandle handle) noexcept : mEngine(engine), mHandle(handle) {}

    ~Local() { reset(); }

    Local(Local const& other) : mEngine(other.mEngine), mHandle(kInvalidHandle) {
        if (other.mEngine != nullptr && other.mHandle != kInvalidHandle) {
            mHandle = other.mEngine->retain(other.mHandle);
        }
    }
    Local(Local&& other) noexcept : mEngine(other.mEngine), mHandle(other.mHandle) {
        other.mEngine = nullptr;
        other.mHandle = kInvalidHandle;
    }

    /// Converting copy from a different tag.
    template <typename Other, typename = std::enable_if_t<!std::is_same_v<Other, Tag>>>
    Local(Local<Other> const& other) : mEngine(other.engine()), mHandle(kInvalidHandle) {
        if (mEngine != nullptr && other.handle() != kInvalidHandle) {
            mHandle = mEngine->retain(other.handle());
        }
    }
    template <typename Other, typename = std::enable_if_t<!std::is_same_v<Other, Tag>>>
    Local(Local<Other>&& other) noexcept : mEngine(other.engine()), mHandle(other.handle()) {
        other.abandon();
    }

    Local& operator=(Local const& other) {
        if (this != &other) {
            reset();
            mEngine = other.mEngine;
            mHandle = (other.mEngine && other.mHandle != kInvalidHandle) ? other.mEngine->retain(other.mHandle)
                                                                        : kInvalidHandle;
        }
        return *this;
    }
    Local& operator=(Local&& other) noexcept {
        if (this != &other) {
            reset();
            mEngine = other.mEngine;
            mHandle = other.mHandle;
            other.mEngine = nullptr;
            other.mHandle = kInvalidHandle;
        }
        return *this;
    }
    template <typename Other, typename = std::enable_if_t<!std::is_same_v<Other, Tag>>>
    Local& operator=(Local<Other> const& other) {
        reset();
        mEngine = other.engine();
        mHandle = (mEngine && other.handle() != kInvalidHandle) ? mEngine->retain(other.handle()) : kInvalidHandle;
        return *this;
    }
    template <typename Other, typename = std::enable_if_t<!std::is_same_v<Other, Tag>>>
    Local& operator=(Local<Other>&& other) noexcept {
        reset();
        mEngine = other.engine();
        mHandle = other.handle();
        other.abandon();
        return *this;
    }

    [[nodiscard]] ValueHandle   handle() const noexcept { return mHandle; }
    [[nodiscard]] ScriptEngine* engine() const noexcept { return mEngine; }
    [[nodiscard]] bool          isValid() const noexcept { return mHandle != kInvalidHandle; }
    explicit operator bool() const noexcept { return isValid(); }

    /// Release ownership without freeing the underlying handle.
    ValueHandle abandon() noexcept {
        ValueHandle h = mHandle;
        mHandle       = kInvalidHandle;
        mEngine       = nullptr;
        return h;
    }

    void reset() {
        if (mEngine != nullptr && mHandle != kInvalidHandle) {
            mEngine->release(mHandle);
        }
        mEngine = nullptr;
        mHandle = kInvalidHandle;
    }

    // -- inspection -------------------------------------------------------
    [[nodiscard]] ValueKind kind() const { return mEngine->kindOf(mHandle); }
    [[nodiscard]] bool isUndefined() const { return kind() == ValueKind::Undefined; }
    [[nodiscard]] bool isNull() const { return kind() == ValueKind::Null; }
    [[nodiscard]] bool isNullOrUndefined() const {
        ValueKind k = kind();
        return k == ValueKind::Null || k == ValueKind::Undefined;
    }
    [[nodiscard]] bool isBool() const { return kind() == ValueKind::Boolean; }
    [[nodiscard]] bool isNumber() const {
        ValueKind k = kind();
        return k == ValueKind::Number || k == ValueKind::Integer;
    }
    [[nodiscard]] bool isInteger() const { return kind() == ValueKind::Integer; }
    [[nodiscard]] bool isString() const { return kind() == ValueKind::String; }
    [[nodiscard]] bool isArray() const { return kind() == ValueKind::Array; }
    [[nodiscard]] bool isFunction() const { return kind() == ValueKind::Function; }
    [[nodiscard]] bool isObject() const {
        ValueKind k = kind();
        return k == ValueKind::Object || k == ValueKind::Array || k == ValueKind::Function ||
               k == ValueKind::NativeObject;
    }
    [[nodiscard]] bool isNativeObject() const { return kind() == ValueKind::NativeObject; }

    // -- conversion -------------------------------------------------------
    [[nodiscard]] bool        toBoolean() const { return mEngine->toBoolean(mHandle); }
    [[nodiscard]] double      toNumber() const { return mEngine->toNumber(mHandle); }
    [[nodiscard]] int64_t     toInteger() const { return mEngine->toInteger(mHandle); }
    [[nodiscard]] std::string toString() const { return mEngine->toStdString(mHandle); }

    // -- object ------------------------------------------------------------
    [[nodiscard]] Local<Value> getProperty(std::string_view key) const {
        return Local<Value>(mEngine, mEngine->getProperty(mHandle, key));
    }
    void setProperty(std::string_view key, ValueHandle value) const { mEngine->setProperty(mHandle, key, value); }
    template <typename U>
    void setProperty(std::string_view key, Local<U> const& value) const {
        mEngine->setProperty(mHandle, key, value.handle());
    }
    [[nodiscard]] bool hasProperty(std::string_view key) const { return mEngine->hasProperty(mHandle, key); }
    [[nodiscard]] std::vector<std::string> getPropertyNames() const { return mEngine->getPropertyNames(mHandle); }

    // -- array -------------------------------------------------------------
    [[nodiscard]] size_t         getLength() const { return mEngine->getLength(mHandle); }
    [[nodiscard]] Local<Value>   getElement(size_t index) const {
        return Local<Value>(mEngine, mEngine->getElement(mHandle, index));
    }
    void setElement(size_t index, ValueHandle value) const { mEngine->setElement(mHandle, index, value); }
    template <typename U>
    void setElement(size_t index, Local<U> const& value) const {
        mEngine->setElement(mHandle, index, value.handle());
    }

    // -- call --------------------------------------------------------------
    [[nodiscard]] Local<Value> call(ValueHandle thisObj, ValueHandle const* args, int argc) const {
        return Local<Value>(mEngine, mEngine->call(mHandle, thisObj, args, argc));
    }
    [[nodiscard]] Local<Value> call() const { return call(kInvalidHandle, nullptr, 0); }
    template <typename... Args>
    [[nodiscard]] Local<Value> call(ValueHandle thisObj, Local<Args> const&... args) const {
        if constexpr (sizeof...(Args) == 0) {
            return call(thisObj, nullptr, 0);
        } else {
            ValueHandle handles[] = {args.handle()...};
            return call(thisObj, handles, static_cast<int>(sizeof...(Args)));
        }
    }
    template <typename... Args>
    [[nodiscard]] Local<Value> call(Local<Args> const&... args) const {
        return call(kInvalidHandle, args...);
    }

private:
    ScriptEngine* mEngine;
    ValueHandle   mHandle;
};

// ---------------------------------------------------------------------------
// Value factories. Each returns an owning Local bound to `engine`.
// ---------------------------------------------------------------------------
inline Local<Value>    makeUndefined(ScriptEngine& engine) { return {&engine, engine.newUndefined()}; }
inline Local<Value>    makeNull(ScriptEngine& engine) { return {&engine, engine.newNull()}; }
inline Local<Boolean>  makeBoolean(ScriptEngine& engine, bool value) { return {&engine, engine.newBoolean(value)}; }
inline Local<Integer>  makeInteger(ScriptEngine& engine, int64_t value) { return {&engine, engine.newInteger(value)}; }
inline Local<Number>   makeNumber(ScriptEngine& engine, double value) { return {&engine, engine.newNumber(value)}; }
inline Local<String>   makeString(ScriptEngine& engine, std::string_view value) { return {&engine, engine.newString(value)}; }
inline Local<Array>    makeArray(ScriptEngine& engine, size_t length = 0) { return {&engine, engine.newArray(length)}; }
inline Local<Object>   makeObject(ScriptEngine& engine) { return {&engine, engine.newObject()}; }
inline Local<Function> makeFunction(ScriptEngine& engine, NativeFunction function) {
    return {&engine, engine.newFunction(std::move(function))};
}
/// Owning handle of the global object.
inline Local<Object>   getGlobal(ScriptEngine& engine) { return {&engine, engine.getGlobalObject()}; }

} // namespace ls::script
