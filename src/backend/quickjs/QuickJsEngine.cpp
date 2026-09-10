#include "backend/quickjs/QuickJsEngine.h"

#include <exception>
#include <string>
#include <utility>

#include "backend/quickjs/QuickJsException.h"
#include "backend/quickjs/QuickJsModuleLoader.h"
#include "ll/api/io/FileUtils.h"
#include "ll/api/utils/StringUtils.h"
#include "script/EngineScope.h"
#include "script/Exception.h"
#include "script/TimerManager.h"

namespace ls::backend::quickjs {

using ls::script::kInvalidHandle;
using ls::script::NativeFunction;
using ls::script::NativeInstance;
using ls::script::ScriptException;
using ls::script::ValueHandle;
using ls::script::ValueKind;

namespace {

JSClassID g_nativeClassId = 0;

void nativeFinalizer(JSRuntime* /*rt*/, JSValue val) {
    void* opaque = JS_GetOpaque(val, g_nativeClassId);
    if (opaque == nullptr) {
        return;
    }
    // Deleting the instance releases its shared_ptr owner, which frees the wrapped
    // object exactly once when this was the last owner (borrowed => no-op).
    delete static_cast<NativeInstance*>(opaque);
}

[[nodiscard]] JSValue runNative(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv, uint16_t magic,
                                bool isConstructor) {
    auto* engine = QuickJsEngine::fromContext(ctx);
    if (engine == nullptr) {
        return throwJsError(ctx, "native call without a bound engine");
    }
    NativeFunction const* fn = engine->findFunction(static_cast<int>(magic));
    if (fn == nullptr || !*fn) {
        return throwJsError(ctx, "native function is missing");
    }

    ValueHandle              thisHandle = isConstructor ? kInvalidHandle : engine->adopt(thisVal);
    std::vector<ValueHandle> argHandles;
    argHandles.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);
    for (int i = 0; i < argc; ++i) {
        argHandles.push_back(engine->adopt(argv[i]));
    }

    JSValue result;
    try {
        ls::script::EngineScope scope(engine); // make the engine reachable via EngineScope::current()
        ValueHandle             ret = (*fn)(*engine, thisHandle, argHandles.data(), argc);
        result            = engine->extract(ret);
        if (isConstructor && !JS_IsObject(result)) {
            JS_FreeValue(ctx, result);
            result = throwJsError(ctx, "native constructor did not return an object");
        }
    } catch (ScriptException const& e) {
        result = throwJsError(ctx, e.fullMessage());
    } catch (std::exception const& e) {
        result = throwJsError(ctx, e.what());
    } catch (...) {
        result = throwJsError(ctx, "unknown native exception");
    }

    if (thisHandle != kInvalidHandle) {
        engine->release(thisHandle);
    }
    for (ValueHandle h : argHandles) {
        engine->release(h);
    }
    return result;
}

JSValue genericTrampoline(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv, uint16_t magic) {
    return runNative(ctx, thisVal, argc, argv, magic, false);
}

JSValue ctorTrampoline(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv, uint16_t magic) {
    return runNative(ctx, newTarget, argc, argv, magic, true);
}

/// QuickJS dispatches CFunctions through the JSCFunctionType union selected by the
/// cproto argument, so passing a *_magic trampoline where a JSCFunction* is
/// expected is the documented pattern. Casting through void* keeps the intent
/// explicit and avoids -Wcast-function-type-mismatch noise.
template <typename Fn>
JSCFunction* asCFunction(Fn* fn) {
    return reinterpret_cast<JSCFunction*>(reinterpret_cast<void*>(fn));
}

} // namespace

JSClassID QuickJsEngine::nativeClassId() noexcept { return g_nativeClassId; }

QuickJsEngine::QuickJsEngine(std::string           pluginName,
                             std::filesystem::path rootDir,
                             std::shared_ptr<ll::io::Logger> logger)
: ls::script::ScriptEngine(std::move(pluginName), std::move(rootDir), std::move(logger)) {
    mRuntime.reset(JS_NewRuntime());
    mContext.reset(JS_NewContext(mRuntime.get()));
    JS_SetContextOpaque(mContext.get(), this);

    if (g_nativeClassId == 0) {
        JS_NewClassID(mRuntime.get(), &g_nativeClassId);
    }
    JSClassDef classDef{"NativeObject", &nativeFinalizer, nullptr, nullptr, nullptr};
    JS_NewClass(mRuntime.get(), g_nativeClassId, &classDef);

    installModuleLoader(*this);
}

QuickJsEngine::~QuickJsEngine() {
    // Release timer function handles while our virtuals still dispatch correctly
    // and the context is alive.
    timers().clearAll();
    mFunctions.clear();
    mHandles.clear(); // underlying JSValues are reclaimed when the context is freed
    if (mContext) {
        JS_SetContextOpaque(mContext.get(), nullptr);
    }
    // mHandles/mFunctions already emptied above; mContext then mRuntime are freed
    // automatically by their unique_ptr deleters (reverse declaration order).
}

// ---------------------------------------------------------------------------
// handle plumbing
// ---------------------------------------------------------------------------
JSValueConst QuickJsEngine::peek(ValueHandle handle) const {
    if (handle == kInvalidHandle) {
        return JS_UNDEFINED;
    }
    JSValue const* slot = mHandles.find(handle);
    return slot != nullptr ? *slot : JS_UNDEFINED;
}

ValueHandle QuickJsEngine::insertOwned(JSValue value) { return mHandles.insert(value); }

ValueHandle QuickJsEngine::adopt(JSValueConst value) { return mHandles.insert(JS_DupValue(mContext.get(), value)); }

JSValue QuickJsEngine::extract(ValueHandle handle) {
    JSValue out = JS_UNDEFINED;
    if (handle != kInvalidHandle && mHandles.extract(handle, out)) {
        return out;
    }
    return JS_UNDEFINED;
}

int QuickJsEngine::storeFunction(NativeFunction function) {
    int id = mNextFunctionId++;
    mFunctions.emplace(id, std::move(function));
    return id;
}

NativeFunction const* QuickJsEngine::findFunction(int magic) const {
    auto it = mFunctions.find(magic);
    return it == mFunctions.end() ? nullptr : &it->second;
}

ValueHandle QuickJsEngine::retain(ValueHandle handle) {
    if (handle == kInvalidHandle) {
        return kInvalidHandle;
    }
    return adopt(peek(handle));
}

void QuickJsEngine::release(ValueHandle handle) {
    if (handle == kInvalidHandle) {
        return;
    }
    if (JSValue* slot = mHandles.find(handle)) {
        JS_FreeValue(mContext.get(), *slot);
    }
    mHandles.erase(handle);
}

// ---------------------------------------------------------------------------
// execution
// ---------------------------------------------------------------------------
ll::Expected<> QuickJsEngine::eval(std::string_view code, std::string_view filename) {
    std::string name(filename);
    JSValue     result = JS_Eval(mContext.get(), code.data(), code.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        return ll::makeStringError(consumeException(mContext.get()));
    }
    JS_FreeValue(mContext.get(), result);
    executePendingJobs();
    return {};
}

ll::Expected<> QuickJsEngine::evalModule(std::string_view code, std::string_view filename) {
    std::string name(filename);
    JSValue     result = JS_Eval(mContext.get(), code.data(), code.size(), name.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result)) {
        return ll::makeStringError(consumeException(mContext.get()));
    }
    // A module evaluates to a promise (top-level await support). Drain the job
    // queue so the body actually runs, then report a rejection - otherwise a
    // top-level throw would be silently swallowed and the plugin would appear to
    // load while having only partially executed.
    executePendingJobs();
    ll::Expected<> outcome{};
    if (JS_IsPromise(result) && JS_PromiseState(mContext.get(), result) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(mContext.get(), result);
        outcome = ll::makeStringError("ES module evaluation failed: " + formatExceptionValue(mContext.get(), reason));
        JS_FreeValue(mContext.get(), reason);
    }
    JS_FreeValue(mContext.get(), result);
    return outcome;
}

ll::Expected<> QuickJsEngine::loadFile(std::filesystem::path const& file, bool asModule) {
    auto content = ll::file_utils::readFile(file, true);
    if (!content) {
        return ll::makeStringError("Failed to read file: " + ll::string_utils::u8str2str(file.u8string()));
    }
    std::string filename = ll::string_utils::u8str2str(file.u8string());
    return asModule ? evalModule(*content, filename) : eval(*content, filename);
}

void QuickJsEngine::executePendingJobs() {
    JSContext* jobCtx = nullptr;
    while (true) {
        int err = JS_ExecutePendingJob(mRuntime.get(), &jobCtx);
        if (err <= 0) {
            if (err < 0 && mLogger) {
                mLogger->error("[Promise] Uncaught exception: {}",
                               consumeException(jobCtx != nullptr ? jobCtx : mContext.get()));
            }
            break;
        }
    }
}

void QuickJsEngine::gc() { JS_RunGC(mRuntime.get()); }

// ---------------------------------------------------------------------------
// inspection / conversion
// ---------------------------------------------------------------------------
ValueKind QuickJsEngine::kindOf(ValueHandle handle) {
    JSValueConst value = peek(handle);
    switch (JS_VALUE_GET_TAG(value)) {
    case JS_TAG_UNDEFINED: return ValueKind::Undefined;
    case JS_TAG_NULL: return ValueKind::Null;
    case JS_TAG_BOOL: return ValueKind::Boolean;
    case JS_TAG_INT: return ValueKind::Integer;
    case JS_TAG_FLOAT64: return ValueKind::Number;
    case JS_TAG_STRING: return ValueKind::String;
    case JS_TAG_SYMBOL: return ValueKind::Symbol;
    case JS_TAG_OBJECT:
        if (JS_IsFunction(mContext.get(), value)) return ValueKind::Function;
        if (JS_IsArray(value)) return ValueKind::Array;
        if (JS_GetOpaque(value, g_nativeClassId) != nullptr) return ValueKind::NativeObject;
        return ValueKind::Object;
    default: return ValueKind::Undefined;
    }
}

bool QuickJsEngine::toBoolean(ValueHandle handle) { return JS_ToBool(mContext.get(), peek(handle)) > 0; }

double QuickJsEngine::toNumber(ValueHandle handle) {
    double value = 0.0;
    if (JS_ToFloat64(mContext.get(), &value, peek(handle)) < 0) {
        JS_FreeValue(mContext.get(), JS_GetException(mContext.get()));
        return 0.0;
    }
    return value;
}

int64_t QuickJsEngine::toInteger(ValueHandle handle) {
    int64_t value = 0;
    if (JS_ToInt64(mContext.get(), &value, peek(handle)) < 0) {
        JS_FreeValue(mContext.get(), JS_GetException(mContext.get()));
        return 0;
    }
    return value;
}

std::string QuickJsEngine::toStdString(ValueHandle handle) {
    size_t      len = 0;
    char const* str = JS_ToCStringLen(mContext.get(), &len, peek(handle));
    if (str == nullptr) {
        JS_FreeValue(mContext.get(), JS_GetException(mContext.get()));
        return {};
    }
    std::string result(str, len);
    JS_FreeCString(mContext.get(), str);
    return result;
}

bool QuickJsEngine::strictEquals(ValueHandle a, ValueHandle b) {
    JSValueConst va = peek(a);
    JSValueConst vb = peek(b);
    int          ta = JS_VALUE_GET_TAG(va);
    int          tb = JS_VALUE_GET_TAG(vb);
    auto isNumber  = [](int tag) { return tag == JS_TAG_INT || tag == JS_TAG_FLOAT64; };
    if (ta != tb && !(isNumber(ta) && isNumber(tb))) {
        return false;
    }
    switch (ta) {
    case JS_TAG_UNDEFINED:
    case JS_TAG_NULL: return true;
    case JS_TAG_BOOL: return JS_ToBool(mContext.get(), va) == JS_ToBool(mContext.get(), vb);
    case JS_TAG_INT:
    case JS_TAG_FLOAT64: {
        double da = 0.0;
        double db = 0.0;
        JS_ToFloat64(mContext.get(), &da, va);
        JS_ToFloat64(mContext.get(), &db, vb);
        return da == db;
    }
    case JS_TAG_STRING: return toStdString(a) == toStdString(b);
    case JS_TAG_OBJECT: return JS_VALUE_GET_PTR(va) == JS_VALUE_GET_PTR(vb);
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// constructors
// ---------------------------------------------------------------------------
ValueHandle QuickJsEngine::newUndefined() { return insertOwned(JS_UNDEFINED); }
ValueHandle QuickJsEngine::newNull() { return insertOwned(JS_NULL); }
ValueHandle QuickJsEngine::newBoolean(bool value) { return insertOwned(JS_NewBool(mContext.get(), value)); }
ValueHandle QuickJsEngine::newInteger(int64_t value) { return insertOwned(JS_NewInt64(mContext.get(), value)); }
ValueHandle QuickJsEngine::newNumber(double value) { return insertOwned(JS_NewFloat64(mContext.get(), value)); }
ValueHandle QuickJsEngine::newString(std::string_view value) {
    return insertOwned(JS_NewStringLen(mContext.get(), value.data(), value.size()));
}
ValueHandle QuickJsEngine::newArray(size_t length) {
    JSValue array = JS_NewArray(mContext.get());
    if (length > 0) {
        JS_SetPropertyStr(mContext.get(), array, "length", JS_NewInt64(mContext.get(), static_cast<int64_t>(length)));
    }
    return insertOwned(array);
}
ValueHandle QuickJsEngine::newObject() { return insertOwned(JS_NewObject(mContext.get())); }

ValueHandle QuickJsEngine::newFunction(NativeFunction function) {
    int     id  = storeFunction(std::move(function));
    JSValue fun = JS_NewCFunction2(mContext.get(), asCFunction(&genericTrampoline), "", 0, JS_CFUNC_generic_magic, id);
    return insertOwned(fun);
}

// ---------------------------------------------------------------------------
// object / array
// ---------------------------------------------------------------------------
ValueHandle QuickJsEngine::getGlobalObject() { return insertOwned(JS_GetGlobalObject(mContext.get())); }

ValueHandle QuickJsEngine::getProperty(ValueHandle object, std::string_view key) {
    std::string name(key);
    JSValue     value = JS_GetPropertyStr(mContext.get(), peek(object), name.c_str());
    if (JS_IsException(value)) {
        JS_FreeValue(mContext.get(), JS_GetException(mContext.get()));
        return insertOwned(JS_UNDEFINED);
    }
    return insertOwned(value);
}

void QuickJsEngine::setProperty(ValueHandle object, std::string_view key, ValueHandle value) {
    std::string name(key);
    // JS_SetPropertyStr consumes the value, so hand it a duplicated reference.
    JS_SetPropertyStr(mContext.get(), peek(object), name.c_str(), JS_DupValue(mContext.get(), peek(value)));
}

void QuickJsEngine::setAccessor(ValueHandle object, std::string_view key, NativeFunction getter, NativeFunction setter) {
    std::string name(key);
    JSValue     getVal = JS_UNDEFINED;
    JSValue     setVal = JS_UNDEFINED;
    if (getter) {
        int id = storeFunction(std::move(getter));
        getVal = JS_NewCFunction2(mContext.get(), asCFunction(&genericTrampoline), name.c_str(), 0,
                                  JS_CFUNC_generic_magic, id);
    }
    if (setter) {
        int id = storeFunction(std::move(setter));
        setVal = JS_NewCFunction2(mContext.get(), asCFunction(&genericTrampoline), name.c_str(), 0,
                                  JS_CFUNC_generic_magic, id);
    }
    JSAtom atom = JS_NewAtom(mContext.get(), name.c_str());
    // An accessor needs the GETSET type plus explicit HAS_GET / HAS_SET bits, or
    // QuickJS installs no getter/setter and the property ends up read-only.
    int flags = JS_PROP_GETSET | JS_PROP_CONFIGURABLE | JS_PROP_HAS_CONFIGURABLE | JS_PROP_ENUMERABLE |
                JS_PROP_HAS_ENUMERABLE;
    if (!JS_IsUndefined(getVal)) {
        flags |= JS_PROP_HAS_GET;
    }
    if (!JS_IsUndefined(setVal)) {
        flags |= JS_PROP_HAS_SET;
    }
    JS_DefineProperty(mContext.get(), peek(object), atom, JS_UNDEFINED, getVal, setVal, flags);
    JS_FreeAtom(mContext.get(), atom);
}

bool QuickJsEngine::hasProperty(ValueHandle object, std::string_view key) {
    std::string name(key);
    JSAtom      atom = JS_NewAtom(mContext.get(), name.c_str());
    int         has  = JS_HasProperty(mContext.get(), peek(object), atom);
    JS_FreeAtom(mContext.get(), atom);
    return has == 1;
}

std::vector<std::string> QuickJsEngine::getPropertyNames(ValueHandle object) {
    std::vector<std::string> result;
    JSPropertyEnum*          tab = nullptr;
    uint32_t                 len = 0;
    if (JS_GetOwnPropertyNames(mContext.get(), &tab, &len, peek(object), JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(mContext.get(), JS_GetException(mContext.get()));
        return result;
    }
    for (uint32_t i = 0; i < len; ++i) {
        char const* str = JS_AtomToCString(mContext.get(), tab[i].atom);
        if (str != nullptr) {
            result.emplace_back(str);
            JS_FreeCString(mContext.get(), str);
        }
        JS_FreeAtom(mContext.get(), tab[i].atom);
    }
    js_free(mContext.get(), tab);
    return result;
}

ValueHandle QuickJsEngine::getElement(ValueHandle array, size_t index) {
    JSValue value = JS_GetPropertyUint32(mContext.get(), peek(array), static_cast<uint32_t>(index));
    if (JS_IsException(value)) {
        JS_FreeValue(mContext.get(), JS_GetException(mContext.get()));
        return insertOwned(JS_UNDEFINED);
    }
    return insertOwned(value);
}

void QuickJsEngine::setElement(ValueHandle array, size_t index, ValueHandle value) {
    JS_SetPropertyUint32(mContext.get(), peek(array), static_cast<uint32_t>(index),
                         JS_DupValue(mContext.get(), peek(value)));
}

size_t QuickJsEngine::getLength(ValueHandle array) {
    ValueHandle lengthHandle = getProperty(array, "length");
    int64_t     length       = toInteger(lengthHandle);
    release(lengthHandle);
    return length < 0 ? 0 : static_cast<size_t>(length);
}

// ---------------------------------------------------------------------------
// call
// ---------------------------------------------------------------------------
ValueHandle QuickJsEngine::call(ValueHandle function, ValueHandle thisObj, ValueHandle const* args, int argc) {
    JSValueConst              thisVal = (thisObj != kInvalidHandle) ? peek(thisObj) : JS_UNDEFINED;
    std::vector<JSValueConst> argv;
    argv.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);
    for (int i = 0; i < argc; ++i) {
        argv.push_back(peek(args[i]));
    }
    JSValue result = JS_Call(mContext.get(), peek(function), thisVal, argc, argv.data());
    if (JS_IsException(result)) {
        throw ScriptException(consumeException(mContext.get()));
    }
    return insertOwned(result);
}

// ---------------------------------------------------------------------------
// native class mechanics
// ---------------------------------------------------------------------------
ValueHandle QuickJsEngine::newPrototype(std::string_view /*className*/, ValueHandle basePrototype) {
    JSValue proto = JS_NewObject(mContext.get());
    if (basePrototype != kInvalidHandle) {
        JS_SetPrototype(mContext.get(), proto, peek(basePrototype));
    }
    return insertOwned(proto);
}

ValueHandle QuickJsEngine::newConstructor(std::string_view name, ValueHandle prototype, NativeFunction ctor) {
    int         id   = storeFunction(std::move(ctor));
    std::string name_(name);
    JSValue     cons = JS_NewCFunction2(mContext.get(), asCFunction(&ctorTrampoline), name_.c_str(), 0,
                                        JS_CFUNC_constructor_magic, id);
    JS_SetConstructor(mContext.get(), cons, peek(prototype));
    return insertOwned(cons);
}

ValueHandle QuickJsEngine::wrapNative(ValueHandle prototype, NativeInstance instance) {
    JSValue object = JS_NewObjectProtoClass(mContext.get(), peek(prototype), g_nativeClassId);
    if (JS_IsException(object)) {
        return kInvalidHandle;
    }
    JS_SetOpaque(object, new NativeInstance(std::move(instance)));
    return insertOwned(object);
}

bool QuickJsEngine::getNative(ValueHandle object, NativeInstance& out) {
    JSValueConst value = peek(object);
    if (JS_VALUE_GET_TAG(value) != JS_TAG_OBJECT) {
        return false;
    }
    void* opaque = JS_GetOpaque(value, g_nativeClassId);
    if (opaque == nullptr) {
        return false;
    }
    out = *static_cast<NativeInstance*>(opaque);
    return true;
}

void QuickJsEngine::setPrototype(ValueHandle object, ValueHandle proto) {
    JS_SetPrototype(mContext.get(), peek(object), peek(proto));
}

} // namespace ls::backend::quickjs
