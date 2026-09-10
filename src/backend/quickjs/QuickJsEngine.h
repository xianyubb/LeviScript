#pragma once

#include <quickjs.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "script/HandleTable.h"
#include "script/ScriptEngine.h"
#include "script/Types.h"

namespace ls::backend::quickjs {

/// The QuickJS implementation of the backend-agnostic ls::script::ScriptEngine.
///
/// Design notes:
///  * Script values are exchanged with the abstraction through a HandleTable that
///    stores one owning `JSValue` per handle.
///  * Native callbacks are stored in a side table and dispatched through a single
///    trampoline; the table index is carried in the QuickJS function "magic".
///  * All wrapped native C++ objects share ONE QuickJS class id and store a
///    `NativeInstance` (data + ClassMeta + ownership token) as their opaque, which
///    is what enables polymorphic unwrapping across an inheritance hierarchy.
class QuickJsEngine final : public ls::script::ScriptEngine {
public:
    QuickJsEngine(std::string pluginName, std::filesystem::path rootDir, std::shared_ptr<ll::io::Logger> logger);
    ~QuickJsEngine() override;

    [[nodiscard]] JSRuntime*                 runtime() const noexcept { return mRuntime.get(); }
    [[nodiscard]] JSContext*                 context() const noexcept { return mContext.get(); }
    /// Plugin root directory used to resolve module specifiers.
    [[nodiscard]] std::filesystem::path const& rootDir() const noexcept { return pluginDir(); }

    /// The QuickJS class id shared by every wrapped native object.
    [[nodiscard]] static JSClassID nativeClassId() noexcept;

    [[nodiscard]] static QuickJsEngine* fromContext(JSContext* ctx) noexcept {
        return static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
    }

    // -- ScriptEngine interface --------------------------------------------
    ll::Expected<> eval(std::string_view code, std::string_view filename) override;
    ll::Expected<> evalModule(std::string_view code, std::string_view filename) override;
    ll::Expected<> loadFile(std::filesystem::path const& file, bool asModule) override;
    void                       executePendingJobs() override;
    void                       gc() override;

    ls::script::ValueHandle retain(ls::script::ValueHandle handle) override;
    void                    release(ls::script::ValueHandle handle) override;

    ls::script::ValueKind kindOf(ls::script::ValueHandle handle) override;
    bool                  toBoolean(ls::script::ValueHandle handle) override;
    double                toNumber(ls::script::ValueHandle handle) override;
    int64_t               toInteger(ls::script::ValueHandle handle) override;
    std::string           toStdString(ls::script::ValueHandle handle) override;
    bool                  strictEquals(ls::script::ValueHandle a, ls::script::ValueHandle b) override;

    ls::script::ValueHandle newUndefined() override;
    ls::script::ValueHandle newNull() override;
    ls::script::ValueHandle newBoolean(bool value) override;
    ls::script::ValueHandle newInteger(int64_t value) override;
    ls::script::ValueHandle newNumber(double value) override;
    ls::script::ValueHandle newString(std::string_view value) override;
    ls::script::ValueHandle newArray(size_t length) override;
    ls::script::ValueHandle newObject() override;
    ls::script::ValueHandle newFunction(ls::script::NativeFunction function) override;

    ls::script::ValueHandle              getGlobalObject() override;
    ls::script::ValueHandle              getProperty(ls::script::ValueHandle object, std::string_view key) override;
    void                                 setProperty(ls::script::ValueHandle object, std::string_view key,
                                                     ls::script::ValueHandle value) override;
    void                                 setAccessor(ls::script::ValueHandle object, std::string_view key,
                                                     ls::script::NativeFunction getter,
                                                     ls::script::NativeFunction setter) override;
    bool                                 hasProperty(ls::script::ValueHandle object, std::string_view key) override;
    std::vector<std::string>             getPropertyNames(ls::script::ValueHandle object) override;
    ls::script::ValueHandle              getElement(ls::script::ValueHandle array, size_t index) override;
    void                                 setElement(ls::script::ValueHandle array, size_t index,
                                                    ls::script::ValueHandle value) override;
    size_t                               getLength(ls::script::ValueHandle array) override;

    ls::script::ValueHandle call(ls::script::ValueHandle function, ls::script::ValueHandle thisObj,
                                 ls::script::ValueHandle const* args, int argc) override;

    ls::script::ValueHandle newPrototype(std::string_view className, ls::script::ValueHandle basePrototype) override;
    ls::script::ValueHandle newConstructor(std::string_view name, ls::script::ValueHandle prototype,
                                           ls::script::NativeFunction ctor) override;
    ls::script::ValueHandle wrapNative(ls::script::ValueHandle prototype, ls::script::NativeInstance instance) override;
    bool                    getNative(ls::script::ValueHandle object, ls::script::NativeInstance& out) override;
    void                    setPrototype(ls::script::ValueHandle object, ls::script::ValueHandle proto) override;

    // -- backend internals (used by the trampolines / module loader) --------
    /// Borrowed view of a handle's JSValue (JS_UNDEFINED for invalid handles).
    [[nodiscard]] JSValueConst peek(ls::script::ValueHandle handle) const;
    /// Insert an already-owned JSValue, returning a fresh owning handle.
    ls::script::ValueHandle insertOwned(JSValue value);
    /// Duplicate a borrowed JSValue into a fresh owning handle.
    ls::script::ValueHandle adopt(JSValueConst value);
    /// Move a handle's JSValue out of the table (transfers ownership); JS_UNDEFINED when invalid.
    JSValue                 extract(ls::script::ValueHandle handle);
    /// Store a native callback, returning its magic id.
    int                     storeFunction(ls::script::NativeFunction function);
    [[nodiscard]] ls::script::NativeFunction const* findFunction(int magic) const;

private:
    // RAII owners for the QuickJS C handles: freed automatically, in the correct
    // order (context before runtime, i.e. reverse declaration order), even if an
    // error is thrown during setup.
    struct RuntimeDeleter {
        void operator()(JSRuntime* rt) const noexcept {
            if (rt != nullptr) JS_FreeRuntime(rt);
        }
    };
    struct ContextDeleter {
        void operator()(JSContext* ctx) const noexcept {
            if (ctx != nullptr) JS_FreeContext(ctx);
        }
    };
    using RuntimePtr = std::unique_ptr<JSRuntime, RuntimeDeleter>;
    using ContextPtr = std::unique_ptr<JSContext, ContextDeleter>;

    RuntimePtr                                     mRuntime;
    ContextPtr                                     mContext;
    ls::script::HandleTable<JSValue>               mHandles;
    std::unordered_map<int, ls::script::NativeFunction> mFunctions;
    int                                            mNextFunctionId = 1;
};

} // namespace ls::backend::quickjs
