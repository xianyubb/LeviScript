#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>

#include "script/ClassRegistry.h"
#include "script/Exception.h"
#include "script/ScriptEngine.h"
#include "script/Types.h"
#include "script/bind/TypeConverter.h"

#include "plugin/include_all.h"

// ---------------------------------------------------------------------------
// The binding framework: automatic function binding + class binding with real
// C++ inheritance support (the feature ScriptX never provided).
//
// Include this single header to bind native symbols into an engine.
// ---------------------------------------------------------------------------

namespace ls::script {

// =========================== function traits ===============================
template <typename T>
struct function_traits : function_traits<decltype(&std::remove_reference_t<T>::operator())> {};

template <typename Ret, typename... Args>
struct function_traits<Ret(Args...)> {
    using signature   = Ret(Args...);
    using return_type = Ret;
    static constexpr std::size_t arity = sizeof...(Args);
};
template <typename Ret, typename... Args>
struct function_traits<Ret (*)(Args...)> : function_traits<Ret(Args...)> {};
// C++17 makes noexcept part of the function type, so a `noexcept` free function
// (pointer) is a distinct type that needs its own specializations.
template <typename Ret, typename... Args>
struct function_traits<Ret(Args...) noexcept> : function_traits<Ret(Args...)> {};
template <typename Ret, typename... Args>
struct function_traits<Ret (*)(Args...) noexcept> : function_traits<Ret(Args...)> {};
template <typename C, typename Ret, typename... Args>
struct function_traits<Ret (C::*)(Args...)> : function_traits<Ret(Args...)> {};
template <typename C, typename Ret, typename... Args>
struct function_traits<Ret (C::*)(Args...) const> : function_traits<Ret(Args...)> {};
template <typename C, typename Ret, typename... Args>
struct function_traits<Ret (C::*)(Args...) noexcept> : function_traits<Ret(Args...)> {};
template <typename C, typename Ret, typename... Args>
struct function_traits<Ret (C::*)(Args...) const noexcept> : function_traits<Ret(Args...)> {};

class ClassBinder;

namespace detail {

[[nodiscard]] inline ValueHandle argAt(ValueHandle const* argv, int argc, std::size_t index) {
    return index < static_cast<std::size_t>(argc) ? argv[index] : kInvalidHandle;
}

/// Defined after ClassBinder; wraps a native pointer without depending on the
/// (still incomplete) ClassBinder type here. Borrowed => never instantiates ~T.
template <typename T>
[[nodiscard]] ValueHandle wrapNativePtrBorrowed(ScriptEngine& engine, T* ptr);

/// setProperty borrows the value handle, so release the one we just created.
inline void defineProperty(ScriptEngine& engine, ValueHandle object, std::string_view key, ValueHandle ownedValue) {
    engine.setProperty(object, key, ownedValue);
    if (ownedValue != kInvalidHandle) {
        engine.release(ownedValue);
    }
}

/// Bridge to the universal pointer system (defined in native/NativePointer.cpp).
/// Any raw pointer that is not a bound native class crosses the boundary as a
/// borrowed NativePointer; this is what lets the generator export pointer types.
ValueHandle wrapRawPointer(ScriptEngine& engine, void* address, std::size_t size);
void*       unwrapRawPointer(ScriptEngine& engine, ValueHandle handle);

/// Convert a call result of type Ret into an owning handle.
template <typename Ret>
[[nodiscard]] ValueHandle writeReturn(ScriptEngine& engine, Ret& result) {
    if constexpr (std::is_void_v<Ret>) {
        return kInvalidHandle;
    } else if constexpr (std::is_lvalue_reference_v<Ret> &&
                         is_native_class_v<std::remove_cv_t<std::remove_reference_t<Ret>>>) {
        return wrapNativePtrBorrowed(engine, std::addressof(result));
    } else {
        using W = std::remove_cv_t<std::remove_reference_t<Ret>>;
        return ToScript<W>::write(engine, std::move(result));
    }
}

template <typename Ret, typename... Args, std::size_t... I>
Ret callFreeRaw(Ret (*fn)(Args...), ScriptEngine& engine, ValueHandle const* argv, int argc, std::index_sequence<I...>) {
    return fn(FromScript<Args>::read(engine, argAt(argv, argc, I))...);
}
// Generic member invocation: the member pointer is a single opaque template
// parameter (so it works for any const / noexcept / ref-qualified form) while the
// signature is supplied separately to drive argument marshalling.
template <typename T, typename MemFn, typename Ret, typename... Args, std::size_t... I>
Ret callMem(T* self, MemFn fn, ScriptEngine& engine, ValueHandle const* argv, int argc, Ret (*)(Args...),
            std::index_sequence<I...>) {
    return (self->*fn)(FromScript<Args>::read(engine, argAt(argv, argc, I))...);
}

template <typename Signature, typename Callable>
struct NativeMaker;

template <typename Ret, typename... Args, typename Callable>
struct NativeMaker<Ret(Args...), Callable> {
    using DecayCallable = std::decay_t<Callable>;

    template <std::size_t... I>
    static ValueHandle invoke(DecayCallable& fn, ScriptEngine& engine, ValueHandle const* argv, int argc,
                              std::index_sequence<I...>) {
        if constexpr (std::is_void_v<Ret>) {
            fn(FromScript<Args>::read(engine, argAt(argv, argc, I))...);
            return kInvalidHandle;
        } else {
            Ret result = fn(FromScript<Args>::read(engine, argAt(argv, argc, I))...);
            return writeReturn<Ret>(engine, result);
        }
    }

    static NativeFunction make(Callable&& callable) {
        auto shared = std::make_shared<DecayCallable>(std::forward<Callable>(callable));
        return [shared](ScriptEngine& engine, ValueHandle, ValueHandle const* argv, int argc) -> ValueHandle {
            return invoke(*shared, engine, argv, argc, std::index_sequence_for<Args...>{});
        };
    }
};

} // namespace detail

/// Turn any callable with a deducible signature into a NativeFunction.
template <typename Callable>
[[nodiscard]] NativeFunction makeNativeFunction(Callable&& callable) {
    using Signature = typename function_traits<std::decay_t<Callable>>::signature;
    return detail::NativeMaker<Signature, Callable>::make(std::forward<Callable>(callable));
}

// ============================== ClassBinder ================================
/// Registers native C++ classes into an engine and marshals instances.
///
/// Inheritance: register base classes before derived ones with
/// `registerClass<Derived, Base>(...)`. The binder then
///   * links the script prototypes (derived.prototype -> base.prototype) so
///     methods/properties are inherited and `instanceof` works, and
///   * records a pointer upcast per edge so a Derived instance can be passed
///     where a Base is expected (with correct pointer adjustment).
class ClassBinder {
public:
    template <typename T>
    static ClassMeta& requireMeta(ScriptEngine& engine) {
        ClassMeta* meta = engine.classes().find(std::type_index(typeid(T)));
        if (meta == nullptr) {
            throw ScriptException(std::string("native class not registered in this engine: ") + typeid(T).name());
        }
        return *meta;
    }

    /// Find the ClassMeta whose constructor is `constructor` (compared by script
    /// object identity). Used by NativePointer.as(Class) to reinterpret an address.
    static ClassMeta* metaFromConstructor(ScriptEngine& engine, ValueHandle constructor) {
        for (auto const& [type, meta] : engine.classes()) {
            if (meta->constructor != kInvalidHandle && engine.strictEquals(meta->constructor, constructor)) {
                return meta.get();
            }
        }
        return nullptr;
    }

    template <typename T, typename Base = void>
    static ClassMeta& registerClass(ScriptEngine& engine, std::string_view name) {
        static_assert(is_native_class_v<T>, "declare the class with LS_NATIVE_CLASS(T) before binding it");
        std::type_index ti(typeid(T));
        if (ClassMeta* existing = engine.classes().find(ti)) {
            return *existing;
        }

        auto owned     = std::make_unique<ClassMeta>(ti);
        owned->name    = std::string(name);

        ValueHandle baseProto = kInvalidHandle;
        if constexpr (!std::is_void_v<Base>) {
            static_assert(std::is_base_of_v<Base, T>, "registerClass<T, Base>: Base must be a base class of T");
            static_assert(is_native_class_v<Base>, "registerClass<T, Base>: Base must be declared with LS_NATIVE_CLASS");
            ClassMeta* baseMeta = engine.classes().find(std::type_index(typeid(Base)));
            if (baseMeta == nullptr) {
                throw ScriptException("registerClass: base class of '" + std::string(name) +
                                      "' must be registered first");
            }
            owned->base   = baseMeta;
            owned->upcast = +[](void* p) -> void* { return static_cast<void*>(static_cast<Base*>(static_cast<T*>(p))); };
            baseProto     = baseMeta->prototype;
        }
        owned->prototype = engine.newPrototype(name, baseProto);

        ClassMeta& meta = engine.classes().add(std::move(owned));
        ClassMeta* self = &meta;
        NativeFunction thunk =
            [self](ScriptEngine& engine2, ValueHandle thisObj, ValueHandle const* argv, int argc) -> ValueHandle {
            if (!self->ctor) {
                throw ScriptException("class '" + self->name + "' is not constructible from script");
            }
            return self->ctor(engine2, thisObj, argv, argc);
        };
        meta.constructor = engine.newConstructor(name, meta.prototype, std::move(thunk));
        engine.setProperty(meta.prototype, "constructor", meta.constructor);
        return meta;
    }

    /// Attach the class constructor to a namespace/global object under `name`.
    template <typename T>
    static void expose(ScriptEngine& engine, ValueHandle target, std::string_view name) {
        ClassMeta& meta = requireMeta<T>(engine);
        engine.setProperty(target, name, meta.constructor);
    }

    // -- methods (generic: any const / noexcept / ref-qualified member) -------
    template <typename T, typename MemFn>
    static void method(ScriptEngine& engine, std::string_view name, MemFn fn) {
        static_assert(std::is_member_function_pointer_v<MemFn>, "method() expects a member function pointer");
        methodImpl<T>(engine, name, fn, static_cast<typename function_traits<MemFn>::signature*>(nullptr));
    }
    template <typename T, typename MemFn, typename Ret, typename... Args>
    static void methodImpl(ScriptEngine& engine, std::string_view name, MemFn fn, Ret (*)(Args...)) {
        ClassMeta&     meta = requireMeta<T>(engine);
        NativeFunction nf   = [fn](ScriptEngine& e, ValueHandle thisObj, ValueHandle const* argv,
                                 int argc) -> ValueHandle {
            T* self = unwrap<T>(e, thisObj);
            if constexpr (std::is_void_v<Ret>) {
                detail::callMem(self, fn, e, argv, argc, static_cast<Ret (*)(Args...)>(nullptr),
                                std::index_sequence_for<Args...>{});
                return kInvalidHandle;
            } else {
                Ret result = detail::callMem(self, fn, e, argv, argc, static_cast<Ret (*)(Args...)>(nullptr),
                                             std::index_sequence_for<Args...>{});
                return detail::writeReturn<Ret>(e, result);
            }
        };
        detail::defineProperty(engine, meta.prototype, name, engine.newFunction(std::move(nf)));
    }

    // -- static methods (attached to the constructor object) ----------------
    template <typename T, typename Callable>
    static void staticMethod(ScriptEngine& engine, std::string_view name, Callable&& callable) {
        ClassMeta& meta = requireMeta<T>(engine);
        detail::defineProperty(engine, meta.constructor, name,
                               engine.newFunction(makeNativeFunction(std::forward<Callable>(callable))));
    }

    // -- accessor properties (generic over getter/setter qualification) ------
    /// Read-only property from a getter member function (any cv/noexcept form).
    template <typename T, typename Getter, typename = std::enable_if_t<std::is_member_function_pointer_v<Getter>>>
    static void property(ScriptEngine& engine, std::string_view name, Getter getter) {
        propertyGetImpl<T>(engine, name, getter, static_cast<typename function_traits<Getter>::signature*>(nullptr));
    }
    template <typename T, typename Getter, typename Ret>
    static void propertyGetImpl(ScriptEngine& engine, std::string_view name, Getter getter, Ret (*)()) {
        ClassMeta& meta = requireMeta<T>(engine);
        engine.setAccessor(meta.prototype, name, makeGetter<T, Ret>(getter), NativeFunction{});
    }
    /// Read/write property from getter + setter member functions.
    template <typename T, typename Getter, typename Setter>
    static void property(ScriptEngine& engine, std::string_view name, Getter getter, Setter setter) {
        propertyGetSetImpl<T>(engine, name, getter, setter,
                              static_cast<typename function_traits<Getter>::signature*>(nullptr),
                              static_cast<typename function_traits<Setter>::signature*>(nullptr));
    }
    template <typename T, typename Getter, typename Setter, typename Ret, typename SetRet, typename Arg>
    static void propertyGetSetImpl(ScriptEngine& engine, std::string_view name, Getter getter, Setter setter,
                                   Ret (*)(), SetRet (*)(Arg)) {
        ClassMeta&     meta = requireMeta<T>(engine);
        NativeFunction set  = [setter](ScriptEngine& e, ValueHandle thisObj, ValueHandle const* argv,
                                      int argc) -> ValueHandle {
            T* self = unwrap<T>(e, thisObj);
            (self->*setter)(FromScript<Arg>::read(e, detail::argAt(argv, argc, 0)));
            return kInvalidHandle;
        };
        engine.setAccessor(meta.prototype, name, makeGetter<T, Ret>(getter), std::move(set));
    }
    template <typename T, typename Ret, typename Getter>
    static NativeFunction makeGetter(Getter getter) {
        return [getter](ScriptEngine& e, ValueHandle thisObj, ValueHandle const*, int) -> ValueHandle {
            T* self = unwrap<T>(e, thisObj);
            if constexpr (std::is_void_v<Ret>) {
                (self->*getter)();
                return kInvalidHandle;
            } else {
                Ret value = (self->*getter)();
                return detail::writeReturn<Ret>(e, value);
            }
        };
    }

    /// Bind a data member as a read/write accessor property (`&T::field`).
    /// Constrained to data members so it never competes with the getter overloads
    /// for pointer-to-member-function arguments.
    template <typename T, typename Field, typename = std::enable_if_t<!std::is_function_v<Field>>>
    static void property(ScriptEngine& engine, std::string_view name, Field T::*field) {
        ClassMeta&     meta = requireMeta<T>(engine);
        NativeFunction get  = [field](ScriptEngine& e, ValueHandle thisObj, ValueHandle const*, int) -> ValueHandle {
            T*    self  = unwrap<T>(e, thisObj);
            Field value = self->*field;
            return detail::writeReturn<Field>(e, value);
        };
        NativeFunction set = [field](ScriptEngine& e, ValueHandle thisObj, ValueHandle const* argv,
                                     int argc) -> ValueHandle {
            T* self   = unwrap<T>(e, thisObj);
            self->*field = FromScript<Field>::read(e, detail::argAt(argv, argc, 0));
            return kInvalidHandle;
        };
        engine.setAccessor(meta.prototype, name, std::move(get), std::move(set));
    }

    // -- constructor --------------------------------------------------------
    /// `factory` must return a heap allocated T* that the script object will own.
    template <typename T, typename... Args>
    static void constructor(ScriptEngine& engine, T* (*factory)(Args...)) {
        ClassMeta& meta = requireMeta<T>(engine);
        meta.ctor       = [factory](ScriptEngine& e, ValueHandle, ValueHandle const* argv, int argc) -> ValueHandle {
            T* object = detail::callFreeRaw(factory, e, argv, argc, std::index_sequence_for<Args...>{});
            return wrapOwned(e, object);
        };
    }

    // -- wrap / unwrap ------------------------------------------------------
    /// Borrowed wrapper: does NOT own the object and never instantiates ~T (no
    /// shared_ptr deleter), so it is safe for PIMPL types whose impl is incomplete
    /// in the header (e.g. ll::event::EventBus).
    template <typename T>
    static ValueHandle wrapBorrowed(ScriptEngine& engine, T* ptr) {
        if (ptr == nullptr) {
            return engine.newNull();
        }
        ClassMeta&     meta = requireMeta<T>(engine);
        NativeInstance instance;
        instance.data = const_cast<void*>(static_cast<void const*>(ptr));
        instance.meta = static_cast<void*>(&meta);
        return engine.wrapNative(meta.prototype, instance);
    }
    /// Owning wrapper: takes ownership via a type-erased shared_ptr (frees the object
    /// exactly once when the last wrapper is GC'd). Requires T to be destructible.
    template <typename T>
    static ValueHandle wrapOwned(ScriptEngine& engine, T* ptr) {
        if (ptr == nullptr) {
            return engine.newNull();
        }
        ClassMeta&     meta = requireMeta<T>(engine);
        NativeInstance instance;
        instance.data  = const_cast<void*>(static_cast<void const*>(ptr));
        instance.meta  = static_cast<void*>(&meta);
        instance.owner = std::shared_ptr<void>(const_cast<std::remove_const_t<T>*>(ptr));
        return engine.wrapNative(meta.prototype, instance);
    }
    /// Runtime-ownership dispatch. NOTE: this compiles both paths, so borrowed
    /// wrappers of PIMPL types must call wrapBorrowed directly (not this).
    template <typename T>
    static ValueHandle wrap(ScriptEngine& engine, T* ptr, bool owned) {
        return owned ? wrapOwned(engine, ptr) : wrapBorrowed(engine, ptr);
    }

    /// Wrap keeping a shared_ptr alive for as long as the script object lives.
    template <typename T>
    static ValueHandle wrapShared(ScriptEngine& engine, std::shared_ptr<T> ptr) {
        if (!ptr) {
            return engine.newNull();
        }
        ClassMeta&     meta = requireMeta<T>(engine);
        NativeInstance instance;
        instance.data  = static_cast<void*>(ptr.get());
        instance.meta  = static_cast<void*>(&meta);
        instance.owner = std::shared_ptr<void>(std::move(ptr)); // shares ownership, deleter preserved
        return engine.wrapNative(meta.prototype, instance);
    }

    template <typename T>
    static T* tryUnwrap(ScriptEngine& engine, ValueHandle handle) noexcept {
        if (handle == kInvalidHandle) {
            return nullptr;
        }
        NativeInstance instance;
        if (!engine.getNative(handle, instance) || instance.data == nullptr || instance.meta == nullptr) {
            return nullptr;
        }
        ClassMeta* target = engine.classes().find(std::type_index(typeid(T)));
        if (target == nullptr) {
            return nullptr;
        }
        auto* actual   = static_cast<ClassMeta*>(instance.meta);
        void* adjusted = actual->castTo(instance.data, target);
        return static_cast<T*>(adjusted);
    }

    template <typename T>
    static T* unwrap(ScriptEngine& engine, ValueHandle handle) {
        T* ptr = tryUnwrap<T>(engine, handle);
        if (ptr == nullptr) {
            ClassMeta* target = engine.classes().find(std::type_index(typeid(T)));
            std::string name  = target ? target->name : typeid(T).name();
            throw ScriptException("expected an instance of '" + name + "'");
        }
        return ptr;
    }
};

namespace detail {
template <typename T>
ValueHandle wrapNativePtrBorrowed(ScriptEngine& engine, T* ptr) {
    return ClassBinder::wrapBorrowed(engine, ptr);
}
} // namespace detail

// ===================== native class type conversions =======================
template <typename T>
struct FromScript<T*, std::enable_if_t<is_native_class_v<T>>> {
    static T* read(ScriptEngine& engine, ValueHandle handle) { return ClassBinder::tryUnwrap<T>(engine, handle); }
};
template <typename T>
struct FromScript<T&, std::enable_if_t<is_native_class_v<T> && !std::is_const_v<T>>> {
    static T& read(ScriptEngine& engine, ValueHandle handle) { return *ClassBinder::unwrap<T>(engine, handle); }
};
template <typename T>
struct FromScript<T const&, std::enable_if_t<is_native_class_v<T>>> {
    static T const& read(ScriptEngine& engine, ValueHandle handle) { return *ClassBinder::unwrap<T>(engine, handle); }
};
template <typename T>
struct FromScript<std::shared_ptr<T>, std::enable_if_t<is_native_class_v<T>>> {
    static std::shared_ptr<T> read(ScriptEngine& engine, ValueHandle handle) {
        T* raw = ClassBinder::tryUnwrap<T>(engine, handle);
        if (raw == nullptr) {
            throw ScriptException("expected an instance for a shared_ptr argument");
        }
        return std::shared_ptr<T>(raw, [](T*) {}); // non owning, valid during the call
    }
};
/// A native class taken BY VALUE: copy the wrapped instance out (T must be copy
/// constructible). Lets the closure binder handle signatures like `f(BlockPos)`.
template <typename T>
struct FromScript<T, std::enable_if_t<is_native_class_v<T> && !std::is_reference_v<T> && !std::is_pointer_v<T>>> {
    static T read(ScriptEngine& engine, ValueHandle handle) { return *ClassBinder::unwrap<T>(engine, handle); }
};

template <typename T>
struct ToScript<T*, std::enable_if_t<is_native_class_v<T>>> {
    static ValueHandle write(ScriptEngine& engine, T* ptr) { return ClassBinder::wrapBorrowed(engine, ptr); }
};
template <typename T>
struct ToScript<T&, std::enable_if_t<is_native_class_v<T>>> {
    static ValueHandle write(ScriptEngine& engine, T& ref) { return ClassBinder::wrapBorrowed(engine, std::addressof(ref)); }
};
template <typename T>
struct ToScript<std::shared_ptr<T>, std::enable_if_t<is_native_class_v<T>>> {
    static ValueHandle write(ScriptEngine& engine, std::shared_ptr<T> ptr) {
        return ClassBinder::wrapShared(engine, std::move(ptr));
    }
};
/// Returning a unique_ptr transfers ownership to the script object: it is deleted
/// when the wrapper is garbage collected. Ideal for static factory functions.
template <typename T>
struct ToScript<std::unique_ptr<T>, std::enable_if_t<is_native_class_v<T>>> {
    static ValueHandle write(ScriptEngine& engine, std::unique_ptr<T> ptr) {
        return ClassBinder::wrapOwned(engine, ptr.release());
    }
};
/// A native class returned BY VALUE: wrap an owned copy (freed on GC). Lets the
/// closure binder handle signatures like `BlockPos east() const`.
template <typename T>
struct ToScript<T, std::enable_if_t<is_native_class_v<T> && !std::is_reference_v<T> && !std::is_pointer_v<T>>> {
    static ValueHandle write(ScriptEngine& engine, T value) {
        return ClassBinder::wrapOwned(engine, new T(std::move(value)));
    }
};

// ---------------------------------------------------------------------------
// Universal pointer fallback: any raw pointer that is NOT a bound native class
// (void*, or a pointer to an opaque/unbound type) is exposed to scripts as a
// borrowed NativePointer, and a NativePointer passed back yields its address.
// `char*` is excluded because it is handled as a string above.
// ---------------------------------------------------------------------------
template <typename T>
struct FromScript<T*, std::enable_if_t<!is_native_class_v<T> && !std::is_same_v<std::remove_const_t<T>, char>>> {
    static T* read(ScriptEngine& engine, ValueHandle handle) {
        return static_cast<T*>(detail::unwrapRawPointer(engine, handle));
    }
};
template <typename T>
struct ToScript<T*, std::enable_if_t<!is_native_class_v<T> && !std::is_same_v<std::remove_const_t<T>, char>>> {
    static ValueHandle write(ScriptEngine& engine, T* pointer) {
        return detail::wrapRawPointer(engine, const_cast<void*>(static_cast<void const*>(pointer)), 0);
    }
};

} // namespace ls::script
