#include <cstdint>
#include <memory>
#include <string>

#include "native/NativeApi.h"
#include "native/NativePointer.h"
#include "script/Exception.h"
#include "script/Local.h"
#include "script/ScriptEngine.h"
#include "script/bind/Bind.h"

// NativePointer crosses the boundary as an owned wrapper; declaring it native
// lets unique_ptr<NativePointer> returns and NativePointer& arguments marshal.
LS_NATIVE_CLASS(::ls::native::NativePointer)

namespace ls::native {

using ls::script::ClassBinder;
using ls::script::ClassMeta;
using ls::script::Function;
using ls::script::getGlobal;
using ls::script::Local;
using ls::script::NativeFunction;
using ls::script::NativeInstance;
using ls::script::Object;
using ls::script::ScriptEngine;
using ls::script::ScriptException;
using ls::script::ValueHandle;

void bindPointerApi(ScriptEngine& engine) {
    Local<Object> global = getGlobal(engine);

    ClassBinder::registerClass<NativePointer>(engine, "NativePointer");

    // -- state --------------------------------------------------------------
    ClassBinder::method<NativePointer>(engine, "isNull", &NativePointer::isNull);
    ClassBinder::method<NativePointer>(engine, "isOwned", &NativePointer::isOwned);
    ClassBinder::method<NativePointer>(engine, "address", &NativePointer::address);
    ClassBinder::property<NativePointer>(engine, "size", &NativePointer::size);

    // -- arithmetic (shares ownership with the base allocation) -------------
    ClassBinder::method<NativePointer>(engine, "add", &NativePointer::add);
    ClassBinder::method<NativePointer>(engine, "sub", &NativePointer::sub);

    // -- typed reads --------------------------------------------------------
    ClassBinder::method<NativePointer>(engine, "getInt8", &NativePointer::getInt8);
    ClassBinder::method<NativePointer>(engine, "getInt16", &NativePointer::getInt16);
    ClassBinder::method<NativePointer>(engine, "getInt32", &NativePointer::getInt32);
    ClassBinder::method<NativePointer>(engine, "getInt64", &NativePointer::getInt64);
    ClassBinder::method<NativePointer>(engine, "getUint8", &NativePointer::getUint8);
    ClassBinder::method<NativePointer>(engine, "getUint16", &NativePointer::getUint16);
    ClassBinder::method<NativePointer>(engine, "getUint32", &NativePointer::getUint32);
    ClassBinder::method<NativePointer>(engine, "getFloat", &NativePointer::getFloat);
    ClassBinder::method<NativePointer>(engine, "getDouble", &NativePointer::getDouble);
    ClassBinder::method<NativePointer>(engine, "getString", &NativePointer::getString);

    // -- typed writes -------------------------------------------------------
    ClassBinder::method<NativePointer>(engine, "setInt8", &NativePointer::setInt8);
    ClassBinder::method<NativePointer>(engine, "setInt16", &NativePointer::setInt16);
    ClassBinder::method<NativePointer>(engine, "setInt32", &NativePointer::setInt32);
    ClassBinder::method<NativePointer>(engine, "setInt64", &NativePointer::setInt64);
    ClassBinder::method<NativePointer>(engine, "setUint8", &NativePointer::setUint8);
    ClassBinder::method<NativePointer>(engine, "setUint16", &NativePointer::setUint16);
    ClassBinder::method<NativePointer>(engine, "setUint32", &NativePointer::setUint32);
    ClassBinder::method<NativePointer>(engine, "setFloat", &NativePointer::setFloat);
    ClassBinder::method<NativePointer>(engine, "setDouble", &NativePointer::setDouble);
    ClassBinder::method<NativePointer>(engine, "setString", &NativePointer::setString);

    // -- static factories ---------------------------------------------------
    ClassBinder::staticMethod<NativePointer>(engine, "alloc", &NativePointer::alloc);
    ClassBinder::staticMethod<NativePointer>(engine, "fromAddress", &NativePointer::fromAddress);

    ClassMeta& meta = ClassBinder::requireMeta<NativePointer>(engine);

    // as(Class): reinterpret the address as an instance of a bound native class
    // (borrowed - the pointed-to object's lifetime is not managed here).
    NativeFunction asFn = [](ScriptEngine& e, ValueHandle thisObj, ValueHandle const* argv, int argc) -> ValueHandle {
        NativePointer* self = ClassBinder::unwrap<NativePointer>(e, thisObj);
        if (argc < 1) {
            throw ScriptException("NativePointer.as(Class) requires a bound class constructor");
        }
        ClassMeta* target = ClassBinder::metaFromConstructor(e, argv[0]);
        if (target == nullptr) {
            throw ScriptException("NativePointer.as(): argument is not a bound native class");
        }
        NativeInstance instance;
        instance.data = self->raw();
        instance.meta = static_cast<void*>(target);
        // Borrowed: leave `owner` empty so reinterpreting an address never takes
        // ownership of (or frees) the pointed-to object.
        return e.wrapNative(target->prototype, instance);
    };
    Local<Function> asLocal(&engine, engine.newFunction(std::move(asFn)));
    engine.setProperty(meta.prototype, "as", asLocal.handle());

    // of(object): borrowed NativePointer to the memory of a bound native instance.
    NativeFunction ofFn = [](ScriptEngine& e, ValueHandle, ValueHandle const* argv, int argc) -> ValueHandle {
        if (argc < 1) {
            throw ScriptException("NativePointer.of(object) requires a native object");
        }
        NativeInstance instance;
        if (!e.getNative(argv[0], instance)) {
            throw ScriptException("NativePointer.of(): argument is not a native object");
        }
        auto pointer = NativePointer::borrow(instance.data, 0);
        return ClassBinder::wrap(e, pointer.release(), true);
    };
    Local<Function> ofLocal(&engine, engine.newFunction(std::move(ofFn)));
    engine.setProperty(meta.constructor, "of", ofLocal.handle());

    // Expose the constructor as a global so scripts can use `new NativePointer`
    // alternatives (alloc/fromAddress/of) and `ptr.as(mc.Player)`.
    ClassBinder::expose<NativePointer>(engine, global.handle(), "NativePointer");
}

} // namespace ls::native
