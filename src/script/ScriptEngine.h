#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ll/api/Expected.h"
#include "ll/api/io/Logger.h"
#include "script/ClassRegistry.h"
#include "script/Types.h"

namespace ls::script {

class TimerManager;

/// Backend-agnostic script engine interface.
///
/// This is the "ScriptX-like" abstraction every language backend implements
/// (QuickJS today; Node.js / Python / Lua can be added under src/backend/*).
/// All value exchange happens through `ValueHandle`s whose lifetime rules are
/// documented per method. Unless stated otherwise a returned handle is *owning*
/// and must be released with release() (or wrapped in a Local<T> which does it).
///
/// Threading: an engine is bound to a single thread (the LeviLamina server
/// thread). Every method must be called on that thread.
class ScriptEngine {
public:
    virtual ~ScriptEngine();

    ScriptEngine(ScriptEngine const&)            = delete;
    ScriptEngine& operator=(ScriptEngine const&) = delete;

    [[nodiscard]] std::string const&  name() const noexcept { return mName; }
    [[nodiscard]] std::filesystem::path const& pluginDir() const noexcept { return mPluginDir; }
    [[nodiscard]] ll::io::Logger&     logger() const noexcept { return *mLogger; }
    void                              setLogger(std::shared_ptr<ll::io::Logger> logger) { mLogger = std::move(logger); }
    [[nodiscard]] ClassRegistry&      classes() noexcept { return mClasses; }
    [[nodiscard]] ClassRegistry const& classes() const noexcept { return mClasses; }

    /// The engine's timer facility (setTimeout / setInterval / ...). Created with
    /// the engine and cleared by the concrete backend before teardown.
    [[nodiscard]] TimerManager& timers() const noexcept;

    // ------------------------------------------------------------------
    // Execution
    // ------------------------------------------------------------------
    /// Evaluate `code` as a classic (global scope) script.
    virtual ll::Expected<> eval(std::string_view code, std::string_view filename = "<eval>") = 0;
    /// Evaluate `code` as an ES module (top level import/export allowed).
    virtual ll::Expected<> evalModule(std::string_view code, std::string_view filename)      = 0;
    /// Read `file` from disk and run it; `asModule` selects ESM vs classic script.
    virtual ll::Expected<> loadFile(std::filesystem::path const& file, bool asModule)        = 0;
    /// Drain deferred jobs (promise reactions, module evaluation steps).
    virtual void           executePendingJobs()                                              = 0;
    /// Run a full garbage collection cycle.
    virtual void           gc()                                                              = 0;

    // ------------------------------------------------------------------
    // Handle lifecycle
    // ------------------------------------------------------------------
    /// Create a second owning handle that references the same value.
    virtual ValueHandle retain(ValueHandle handle) = 0;
    /// Destroy an owning handle.
    virtual void        release(ValueHandle handle) = 0;

    // ------------------------------------------------------------------
    // Inspection / conversion (borrowed reads, no ownership change)
    // ------------------------------------------------------------------
    virtual ValueKind   kindOf(ValueHandle handle)        = 0;
    virtual bool        toBoolean(ValueHandle handle)     = 0;
    virtual double      toNumber(ValueHandle handle)      = 0;
    virtual int64_t     toInteger(ValueHandle handle)     = 0;
    virtual std::string toStdString(ValueHandle handle)   = 0;
    virtual bool        strictEquals(ValueHandle a, ValueHandle b) = 0;

    // ------------------------------------------------------------------
    // Constructors (return owning handles)
    // ------------------------------------------------------------------
    virtual ValueHandle newUndefined()                       = 0;
    virtual ValueHandle newNull()                            = 0;
    virtual ValueHandle newBoolean(bool value)               = 0;
    virtual ValueHandle newInteger(int64_t value)            = 0;
    virtual ValueHandle newNumber(double value)              = 0;
    virtual ValueHandle newString(std::string_view value)    = 0;
    virtual ValueHandle newArray(size_t length = 0)          = 0;
    virtual ValueHandle newObject()                          = 0;
    virtual ValueHandle newFunction(NativeFunction function) = 0;

    // ------------------------------------------------------------------
    // Object / Array
    // ------------------------------------------------------------------
    virtual ValueHandle              getGlobalObject()                                       = 0;
    virtual ValueHandle              getProperty(ValueHandle object, std::string_view key)   = 0;
    virtual void                     setProperty(ValueHandle object, std::string_view key, ValueHandle value) = 0;
    /// Define an accessor property. An empty getter/setter means "not present".
    /// The setter receives the new value as argv[0].
    virtual void                     setAccessor(ValueHandle object, std::string_view key, NativeFunction getter,
                                                 NativeFunction setter) = 0;
    virtual bool                     hasProperty(ValueHandle object, std::string_view key)   = 0;
    virtual std::vector<std::string> getPropertyNames(ValueHandle object)                    = 0;
    virtual ValueHandle              getElement(ValueHandle array, size_t index)             = 0;
    virtual void                     setElement(ValueHandle array, size_t index, ValueHandle value) = 0;
    virtual size_t                   getLength(ValueHandle array)                            = 0;

    // ------------------------------------------------------------------
    // Call
    // ------------------------------------------------------------------
    /// Call `function`. `thisObj` and `args` are borrowed; the result is owning.
    /// Throws ScriptException when the callee throws.
    virtual ValueHandle call(ValueHandle function, ValueHandle thisObj, ValueHandle const* args, int argc) = 0;

    // ------------------------------------------------------------------
    // Native class mechanics (implemented by each backend using its own
    // prototype / opaque facilities; the inheritance *logic* lives in the
    // backend-agnostic class binder on top of these primitives).
    // ------------------------------------------------------------------
    /// Create a fresh prototype object. When `basePrototype` is valid the new
    /// prototype's [[Prototype]] is set to it, establishing script inheritance.
    virtual ValueHandle newPrototype(std::string_view className, ValueHandle basePrototype) = 0;
    /// Create a constructor function whose `.prototype` is `prototype`.
    virtual ValueHandle newConstructor(std::string_view name, ValueHandle prototype, NativeFunction ctor) = 0;
    /// Create an object with the given prototype that wraps `instance`.
    virtual ValueHandle wrapNative(ValueHandle prototype, NativeInstance instance)          = 0;
    /// Read back the NativeInstance of a wrapped object; false when not native.
    virtual bool        getNative(ValueHandle object, NativeInstance& out)                  = 0;
    /// Set the [[Prototype]] of an existing object.
    virtual void        setPrototype(ValueHandle object, ValueHandle proto)                 = 0;

protected:
    ScriptEngine(std::string name, std::filesystem::path pluginDir, std::shared_ptr<ll::io::Logger> logger);

    std::string                     mName;
    std::filesystem::path           mPluginDir;
    std::shared_ptr<ll::io::Logger> mLogger;
    ClassRegistry                   mClasses;
    std::unique_ptr<TimerManager>   mTimers;
};

} // namespace ls::script
