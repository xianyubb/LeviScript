#pragma once

#include <cstdint>
#include <functional>
#include <memory>

// Core backend-agnostic types of the LeviScript abstraction layer.
//
// This layer plays the same role ScriptX plays for LegacyScriptEngine, but it is
// written from scratch and - unlike ScriptX - it fully supports exposing native
// C++ class *inheritance* hierarchies to the script side.
//
// Values are exchanged between the abstraction and a concrete backend through
// opaque `ValueHandle`s. A handle always belongs to exactly one ScriptEngine and
// is either *owning* (the holder must release it) or *borrowed* (valid only for
// the duration of a native callback). Every API documents which one it uses.

namespace ls::script {

class ScriptEngine;

using ValueHandle = uint32_t;

/// Reserved sentinel: handle index 0 is never handed out by a backend.
inline constexpr ValueHandle kInvalidHandle = 0;

/// Runtime classification of a script value.
enum class ValueKind : uint8_t {
    Undefined,
    Null,
    Boolean,
    Integer,
    Number,
    String,
    Symbol,
    Array,
    Function,
    /// A plain object that does not wrap a native C++ instance.
    Object,
    /// An object that wraps a native C++ instance (created by the class binder).
    NativeObject,
};

/// A native callback that can be exposed to scripts.
///
/// It always runs on the engine thread (the server thread for LeviLamina).
/// @param engine   the engine the call happens on
/// @param thisObj  borrowed handle of the receiver (`new.target` style ctor calls
///                 pass an invalid handle)
/// @param argv     borrowed handles of the arguments, valid only during the call
/// @param argc     number of arguments
/// @return         an *owning* handle of the result, or kInvalidHandle for undefined
/// @throws         ScriptException (or any std::exception) to signal a script error;
///                 the backend converts it into a native script exception.
using NativeFunction =
    std::function<ValueHandle(ScriptEngine& engine, ValueHandle thisObj, ValueHandle const* argv, int argc)>;

/// Data attached to a script object that wraps a native C++ instance.
///
/// Ownership is expressed with a smart pointer: `owner` keeps the wrapped object
/// alive and frees it (with the correct type-specific deleter, captured when the
/// shared_ptr was created) when the last wrapper goes away. It is empty for
/// borrowed wrappers, so foreign memory is never freed and nothing leaks.
struct NativeInstance {
    /// Borrowed view of the C++ object, as the most derived bound type.
    void* data = nullptr;
    /// Non-owning pointer to the `ClassMeta` describing the dynamic type.
    void* meta = nullptr;
    /// Owning smart pointer; empty => borrowed (non-owning) wrapper.
    std::shared_ptr<void> owner;
};

} // namespace ls::script
