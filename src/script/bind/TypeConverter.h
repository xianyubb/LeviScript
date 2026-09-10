#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "script/Exception.h"
#include "script/Local.h"
#include "script/ScriptEngine.h"
#include "script/Types.h"

// Automatic marshalling between C++ types and script values.
//
// `FromScript<T>::read(engine, handle)` converts a (borrowed) script value into a
// C++ value of type T; `ToScript<T>::write(engine, value)` produces an *owning*
// handle from a C++ value. The function/class binders are built entirely on these
// two traits, which is what makes binding a native symbol a one-liner.
//
// Native (wrapped) classes opt in through the `is_native_class` trait; their
// pointer / reference / shared_ptr conversions are added by ClassBinder.h.

namespace ls::script {

template <typename T>
struct is_native_class : std::false_type {};
template <typename T>
inline constexpr bool is_native_class_v = is_native_class<std::remove_cv_t<T>>::value;

template <typename T, typename = void>
struct FromScript;
template <typename T, typename = void>
struct ToScript;

// ---------------------------------------------------------------------------
// bool
// ---------------------------------------------------------------------------
template <>
struct FromScript<bool> {
    static bool read(ScriptEngine& engine, ValueHandle handle) { return engine.toBoolean(handle); }
};
template <>
struct ToScript<bool> {
    static ValueHandle write(ScriptEngine& engine, bool value) { return engine.newBoolean(value); }
};

// ---------------------------------------------------------------------------
// integral (excluding bool)
// ---------------------------------------------------------------------------
template <typename T>
struct FromScript<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    static T read(ScriptEngine& engine, ValueHandle handle) { return static_cast<T>(engine.toInteger(handle)); }
};
template <typename T>
struct ToScript<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    static ValueHandle write(ScriptEngine& engine, T value) { return engine.newInteger(static_cast<int64_t>(value)); }
};

// ---------------------------------------------------------------------------
// floating point
// ---------------------------------------------------------------------------
template <typename T>
struct FromScript<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static T read(ScriptEngine& engine, ValueHandle handle) { return static_cast<T>(engine.toNumber(handle)); }
};
template <typename T>
struct ToScript<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static ValueHandle write(ScriptEngine& engine, T value) { return engine.newNumber(static_cast<double>(value)); }
};

// ---------------------------------------------------------------------------
// enums <-> integers
// ---------------------------------------------------------------------------
template <typename T>
struct FromScript<T, std::enable_if_t<std::is_enum_v<T>>> {
    static T read(ScriptEngine& engine, ValueHandle handle) {
        return static_cast<T>(engine.toInteger(handle));
    }
};
template <typename T>
struct ToScript<T, std::enable_if_t<std::is_enum_v<T>>> {
    static ValueHandle write(ScriptEngine& engine, T value) {
        return engine.newInteger(static_cast<int64_t>(value));
    }
};

// ---------------------------------------------------------------------------
// strings
// ---------------------------------------------------------------------------
template <>
struct FromScript<std::string> {
    static std::string read(ScriptEngine& engine, ValueHandle handle) { return engine.toStdString(handle); }
};
template <>
struct ToScript<std::string> {
    static ValueHandle write(ScriptEngine& engine, std::string const& value) { return engine.newString(value); }
};
template <>
struct ToScript<std::string_view> {
    static ValueHandle write(ScriptEngine& engine, std::string_view value) { return engine.newString(value); }
};
/// A std::string_view parameter borrows memory that must outlive the call. We
/// materialize the script string into a small thread-local ring so that several
/// string_view arguments of one call stay valid simultaneously (each read takes the
/// next slot). Limitation: a single call with more than kSlots string_view arguments,
/// or a callee that stores the view beyond the call, is not supported.
template <>
struct FromScript<std::string_view> {
    static std::string_view read(ScriptEngine& engine, ValueHandle handle) {
        constexpr std::size_t  kSlots = 16;
        thread_local std::string ring[kSlots];
        thread_local std::size_t next = 0;
        std::string&             slot = ring[next];
        next                        = (next + 1) % kSlots;
        slot                        = engine.toStdString(handle);
        return std::string_view(slot);
    }
};
template <>
struct ToScript<char const*> {
    static ValueHandle write(ScriptEngine& engine, char const* value) { return engine.newString(value ? value : ""); }
};
template <>
struct ToScript<char*> {
    static ValueHandle write(ScriptEngine& engine, char* value) { return engine.newString(value ? value : ""); }
};

// ---------------------------------------------------------------------------
// std::filesystem::path <-> string (pervasive across the LL API: getModDir, ...)
// Marshalled as a UTF-8 string so path-returning/taking API functions bind.
// ---------------------------------------------------------------------------
template <>
struct FromScript<std::filesystem::path> {
    static std::filesystem::path read(ScriptEngine& engine, ValueHandle handle) {
        std::string bytes = engine.toStdString(handle);
        return std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(bytes.data()), bytes.size()));
    }
};
template <>
struct ToScript<std::filesystem::path> {
    static ValueHandle write(ScriptEngine& engine, std::filesystem::path const& value) {
        std::u8string bytes = value.u8string();
        return engine.newString(std::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()));
    }
};

// ---------------------------------------------------------------------------
// std::optional<T>  (null / undefined -> nullopt)
// ---------------------------------------------------------------------------
template <typename T>
struct FromScript<std::optional<T>> {
    static std::optional<T> read(ScriptEngine& engine, ValueHandle handle) {
        ValueKind kind = engine.kindOf(handle);
        if (kind == ValueKind::Undefined || kind == ValueKind::Null) {
            return std::nullopt;
        }
        return FromScript<T>::read(engine, handle);
    }
};
template <typename T>
struct ToScript<std::optional<T>> {
    static ValueHandle write(ScriptEngine& engine, std::optional<T> const& value) {
        if (!value.has_value()) {
            return engine.newNull();
        }
        return ToScript<T>::write(engine, *value);
    }
};

// ---------------------------------------------------------------------------
// std::vector<T>  <->  script Array
// ---------------------------------------------------------------------------
template <typename T>
struct FromScript<std::vector<T>> {
    static std::vector<T> read(ScriptEngine& engine, ValueHandle handle) {
        if (engine.kindOf(handle) != ValueKind::Array) {
            throw ScriptException("expected an Array argument");
        }
        std::vector<T> result;
        size_t         length = engine.getLength(handle);
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            ValueHandle element = engine.getElement(handle, i);
            result.push_back(FromScript<T>::read(engine, element));
            engine.release(element);
        }
        return result;
    }
};
template <typename T>
struct ToScript<std::vector<T>> {
    static ValueHandle write(ScriptEngine& engine, std::vector<T> const& value) {
        ValueHandle array = engine.newArray(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            ValueHandle element = ToScript<T>::write(engine, value[i]);
            engine.setElement(array, i, element);
            engine.release(element);
        }
        return array;
    }
};

// ---------------------------------------------------------------------------
// pass-through: raw handle (ownership is transferred on write)
// ---------------------------------------------------------------------------
template <>
struct FromScript<ValueHandle> {
    static ValueHandle read(ScriptEngine&, ValueHandle handle) { return handle; }
};
template <>
struct ToScript<ValueHandle> {
    static ValueHandle write(ScriptEngine&, ValueHandle handle) { return handle; }
};

// ---------------------------------------------------------------------------
// pass-through: Local<Tag> (write transfers ownership out of the Local)
// ---------------------------------------------------------------------------
template <typename Tag>
struct FromScript<Local<Tag>> {
    static Local<Tag> read(ScriptEngine& engine, ValueHandle handle) {
        return Local<Tag>(&engine, handle == kInvalidHandle ? kInvalidHandle : engine.retain(handle));
    }
};
template <typename Tag>
struct ToScript<Local<Tag>> {
    static ValueHandle write(ScriptEngine&, Local<Tag> value) { return value.abandon(); }
};

// ---------------------------------------------------------------------------
// generic reference collapsing for non-native value types:
//   `T const&` / `T&&` are read exactly like `T` (the produced prvalue binds to
//   the reference for the duration of the call, which is safe).
// ---------------------------------------------------------------------------
template <typename T>
struct FromScript<T const&, std::enable_if_t<!is_native_class_v<T>>> : FromScript<T> {};
template <typename T>
struct FromScript<T&&, std::enable_if_t<!is_native_class_v<T>>> : FromScript<T> {};

} // namespace ls::script

// Declare that a C++ type is exposed to scripts through the class binder.
// The argument must be a type name that is valid inside namespace ls::script
// (fully qualified names such as `::mc::Player` are recommended). It is variadic
// so a specialization with commas in its template argument list (e.g.
// `::ll::Foo<::A, ::B>`) is not split into multiple macro arguments.
#define LS_NATIVE_CLASS(...)                                                                                           \
    namespace ls::script {                                                                                             \
    template <>                                                                                                        \
    struct is_native_class<__VA_ARGS__> : ::std::true_type {};                                                         \
    }
