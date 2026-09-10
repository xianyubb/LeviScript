#include "native/NativePointer.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "ll/api/utils/StringUtils.h"
#include "script/Exception.h"
#include "script/bind/Bind.h"

namespace ls::native {

using ls::script::ScriptException;

namespace {
[[nodiscard]] std::string outOfBounds(std::size_t offset, std::size_t nbytes, std::size_t capacity) {
    return "NativePointer: access out of bounds (offset=" + std::to_string(offset) +
           ", bytes=" + std::to_string(nbytes) + ", capacity=" + std::to_string(capacity) + ")";
}
} // namespace

NativePointer::NativePointer(std::shared_ptr<void> owner, void* ptr, std::size_t size)
: mOwner(std::move(owner)), mPtr(ptr), mSize(size) {}

std::unique_ptr<NativePointer> NativePointer::alloc(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    auto*                 buffer = new std::uint8_t[size](); // zero-initialized
    std::shared_ptr<void> owner(buffer, [](void* p) { delete[] static_cast<std::uint8_t*>(p); });
    return std::make_unique<NativePointer>(std::move(owner), buffer, size);
}

std::unique_ptr<NativePointer> NativePointer::fromAddress(std::string const& address, std::size_t size) {
    std::uint64_t value = 0;
    try {
        value = std::stoull(address, nullptr, 0); // base 0 handles the 0x prefix
    } catch (std::exception const&) {
        throw ScriptException("NativePointer.fromAddress: invalid address '" + address + "'");
    }
    return std::make_unique<NativePointer>(
        std::shared_ptr<void>{},
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(value)),
        size
    );
}

std::unique_ptr<NativePointer> NativePointer::borrow(void* address, std::size_t size) {
    return std::make_unique<NativePointer>(std::shared_ptr<void>{}, address, size);
}

std::string NativePointer::address() const {
    return ll::string_utils::intToHexStr(reinterpret_cast<std::uint64_t>(mPtr), false, false, true);
}

std::unique_ptr<NativePointer> NativePointer::add(std::int64_t bytes) const {
    auto*       moved   = static_cast<char*>(mPtr) + bytes;
    std::size_t newSize = 0;
    if (mSize > 0 && bytes >= 0 && static_cast<std::size_t>(bytes) <= mSize) {
        newSize = mSize - static_cast<std::size_t>(bytes);
    }
    return std::make_unique<NativePointer>(mOwner, moved, newSize); // shares ownership
}

std::unique_ptr<NativePointer> NativePointer::sub(std::int64_t bytes) const { return add(-bytes); }

void NativePointer::checkBounds(std::size_t offset, std::size_t nbytes) const {
    if (mPtr == nullptr) {
        throw ScriptException("NativePointer: dereference of a null pointer");
    }
    if (mSize == 0) {
        return; // borrowed with unknown extent: best effort, cannot verify
    }
    if (offset > mSize || nbytes > mSize - offset) {
        throw ScriptException(outOfBounds(offset, nbytes, mSize));
    }
}

template <typename T>
T NativePointer::readRaw(std::size_t offset) const {
    checkBounds(offset, sizeof(T));
    T value;
    std::memcpy(&value, static_cast<char const*>(mPtr) + offset, sizeof(T));
    return value;
}

template <typename T>
void NativePointer::writeRaw(std::size_t offset, T value) {
    checkBounds(offset, sizeof(T));
    std::memcpy(static_cast<char*>(mPtr) + offset, &value, sizeof(T));
}

std::int64_t NativePointer::getInt8(std::size_t o) const { return readRaw<std::int8_t>(o); }
std::int64_t NativePointer::getInt16(std::size_t o) const { return readRaw<std::int16_t>(o); }
std::int64_t NativePointer::getInt32(std::size_t o) const { return readRaw<std::int32_t>(o); }
std::int64_t NativePointer::getInt64(std::size_t o) const { return readRaw<std::int64_t>(o); }
std::int64_t NativePointer::getUint8(std::size_t o) const { return readRaw<std::uint8_t>(o); }
std::int64_t NativePointer::getUint16(std::size_t o) const { return readRaw<std::uint16_t>(o); }
std::int64_t NativePointer::getUint32(std::size_t o) const { return readRaw<std::uint32_t>(o); }
double       NativePointer::getFloat(std::size_t o) const { return readRaw<float>(o); }
double       NativePointer::getDouble(std::size_t o) const { return readRaw<double>(o); }

void NativePointer::setInt8(std::size_t o, std::int64_t v) { writeRaw<std::int8_t>(o, static_cast<std::int8_t>(v)); }
void NativePointer::setInt16(std::size_t o, std::int64_t v) { writeRaw<std::int16_t>(o, static_cast<std::int16_t>(v)); }
void NativePointer::setInt32(std::size_t o, std::int64_t v) { writeRaw<std::int32_t>(o, static_cast<std::int32_t>(v)); }
void NativePointer::setInt64(std::size_t o, std::int64_t v) { writeRaw<std::int64_t>(o, v); }
void NativePointer::setUint8(std::size_t o, std::int64_t v) { writeRaw<std::uint8_t>(o, static_cast<std::uint8_t>(v)); }
void NativePointer::setUint16(std::size_t o, std::int64_t v) { writeRaw<std::uint16_t>(o, static_cast<std::uint16_t>(v)); }
void NativePointer::setUint32(std::size_t o, std::int64_t v) { writeRaw<std::uint32_t>(o, static_cast<std::uint32_t>(v)); }
void NativePointer::setFloat(std::size_t o, double v) { writeRaw<float>(o, static_cast<float>(v)); }
void NativePointer::setDouble(std::size_t o, double v) { writeRaw<double>(o, v); }

std::string NativePointer::getString(std::size_t offset, std::size_t length) const {
    if (mPtr == nullptr) {
        throw ScriptException("NativePointer: dereference of a null pointer");
    }
    if (mSize > 0 && offset >= mSize) {
        throw ScriptException(outOfBounds(offset, length, mSize));
    }
    char const* base = static_cast<char const*>(mPtr) + offset;
    if (length > 0) {
        checkBounds(offset, length);
        return std::string(base, length);
    }
    std::size_t limit = (mSize > 0) ? (mSize - offset) : (static_cast<std::size_t>(1) << 20);
    std::size_t count = 0;
    while (count < limit && base[count] != '\0') {
        ++count;
    }
    return std::string(base, count);
}

void NativePointer::setString(std::size_t offset, std::string const& value) {
    checkBounds(offset, value.size() + 1);
    std::memcpy(static_cast<char*>(mPtr) + offset, value.c_str(), value.size() + 1);
}

} // namespace ls::native

// ---------------------------------------------------------------------------
// Pointer bridge used by the generic (non-native) T* conversions in Bind.h: any
// raw pointer crossing the boundary becomes a borrowed NativePointer, and a
// NativePointer handed back to native code yields its address. This is what lets
// the binding generator export pointer-typed signatures to scripts.
// ---------------------------------------------------------------------------
namespace ls::script::detail {

ValueHandle wrapRawPointer(ScriptEngine& engine, void* address, std::size_t size) {
    auto pointer = ls::native::NativePointer::borrow(address, size);
    return ClassBinder::wrap(engine, pointer.release(), true);
}

void* unwrapRawPointer(ScriptEngine& engine, ValueHandle handle) {
    auto* pointer = ClassBinder::tryUnwrap<ls::native::NativePointer>(engine, handle);
    return pointer != nullptr ? pointer->raw() : nullptr;
}

} // namespace ls::script::detail
