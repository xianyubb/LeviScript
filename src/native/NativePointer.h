#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ls::native {

/// A universal, script-visible native pointer.
///
/// Ownership / lifetime (the "no leaks, prefer smart pointers" contract):
///  * An *owning* pointer created by alloc() keeps its buffer alive through a
///    `std::shared_ptr<void>` with a real deleter. When the last NativePointer
///    referring to it is garbage collected, the buffer is freed automatically.
///  * add()/sub() return a new NativePointer that *shares* the same owner, so a
///    derived pointer keeps the base allocation alive and none of them leak.
///  * A *borrowed* pointer (fromAddress(), of(), or a raw pointer handed back by
///    a bound function) has an empty owner: destroying it never frees foreign
///    memory, so there is no double free and no leak.
///
/// The class itself is exposed to scripts through the class binder, and the
/// wrapper object is owned by the script object (deleted on GC), so neither the
/// NativePointer instance nor its buffer can leak.
class NativePointer {
public:
    NativePointer() = default;
    NativePointer(std::shared_ptr<void> owner, void* ptr, std::size_t size);

    /// Allocate `size` zero-initialized bytes owned by the returned pointer.
    [[nodiscard]] static std::unique_ptr<NativePointer> alloc(std::size_t size);
    /// Wrap an existing address parsed from a hex string (borrowed, non-owning).
    [[nodiscard]] static std::unique_ptr<NativePointer> fromAddress(std::string const& address, std::size_t size);
    /// Wrap an existing address (borrowed, non-owning).
    [[nodiscard]] static std::unique_ptr<NativePointer> borrow(void* address, std::size_t size);

    [[nodiscard]] void*       raw() const noexcept { return mPtr; }
    [[nodiscard]] bool        isNull() const noexcept { return mPtr == nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return mSize; }
    [[nodiscard]] bool        isOwned() const noexcept { return static_cast<bool>(mOwner); }
    [[nodiscard]] std::string address() const;

    /// Pointer arithmetic; the result shares this pointer's owner (if any).
    [[nodiscard]] std::unique_ptr<NativePointer> add(std::int64_t bytes) const;
    [[nodiscard]] std::unique_ptr<NativePointer> sub(std::int64_t bytes) const;

    // -- typed access (bounds-checked when the size is known) ---------------
    [[nodiscard]] std::int64_t getInt8(std::size_t offset) const;
    [[nodiscard]] std::int64_t getInt16(std::size_t offset) const;
    [[nodiscard]] std::int64_t getInt32(std::size_t offset) const;
    [[nodiscard]] std::int64_t getInt64(std::size_t offset) const;
    [[nodiscard]] std::int64_t getUint8(std::size_t offset) const;
    [[nodiscard]] std::int64_t getUint16(std::size_t offset) const;
    [[nodiscard]] std::int64_t getUint32(std::size_t offset) const;
    [[nodiscard]] double       getFloat(std::size_t offset) const;
    [[nodiscard]] double       getDouble(std::size_t offset) const;

    void setInt8(std::size_t offset, std::int64_t value);
    void setInt16(std::size_t offset, std::int64_t value);
    void setInt32(std::size_t offset, std::int64_t value);
    void setInt64(std::size_t offset, std::int64_t value);
    void setUint8(std::size_t offset, std::int64_t value);
    void setUint16(std::size_t offset, std::int64_t value);
    void setUint32(std::size_t offset, std::int64_t value);
    void setFloat(std::size_t offset, double value);
    void setDouble(std::size_t offset, double value);

    /// Read a string. `length == 0` reads up to the NUL terminator.
    [[nodiscard]] std::string getString(std::size_t offset, std::size_t length) const;
    /// Write a NUL-terminated string.
    void setString(std::size_t offset, std::string const& value);

    [[nodiscard]] std::shared_ptr<void> const& owner() const noexcept { return mOwner; }

private:
    void checkBounds(std::size_t offset, std::size_t nbytes) const; // throws ScriptException

    template <typename T>
    [[nodiscard]] T readRaw(std::size_t offset) const;
    template <typename T>
    void writeRaw(std::size_t offset, T value);

    std::shared_ptr<void> mOwner; // empty => borrowed
    void*                 mPtr = nullptr;
    std::size_t           mSize = 0;
};

} // namespace ls::native
