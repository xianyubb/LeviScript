#pragma once

#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include "script/Types.h"

namespace ls::script {

/// Static description of one native C++ class exposed to scripts.
///
/// This is what makes real inheritance work (something ScriptX never supported):
/// every meta remembers its base meta and how to adjust a pointer from the derived
/// type to the base type. Combined with a script prototype chain
/// (`derived.prototype -> base.prototype`) this yields:
///   * inherited methods / properties on the script side
///   * `derivedInstance instanceof Base === true`
///   * passing a Derived where a Base is expected (with correct pointer adjustment)
struct ClassMeta {
    std::string     name;
    std::type_index type;

    /// Immediate base class meta, or nullptr for a root class.
    ClassMeta* base = nullptr;

    /// Converts a pointer of *this* type into a pointer of its immediate `base`
    /// type (i.e. `static_cast<Base*>(static_cast<This*>(p))`). Null for roots.
    void* (*upcast)(void* ptr) = nullptr;

    /// Factory invoked when scripts run `new Class(...)`. When empty the class is
    /// not constructible from script. Returns an owning handle of the new instance.
    NativeFunction ctor;

    /// Owning handles to the script prototype / constructor objects. They live as
    /// long as the engine, so they are never released individually.
    ValueHandle prototype   = kInvalidHandle;
    ValueHandle constructor = kInvalidHandle;

    explicit ClassMeta(std::type_index type) : type(type) {}

    /// True when `this` is `other` or derives from it.
    [[nodiscard]] bool derivesFrom(ClassMeta const* other) const noexcept {
        for (ClassMeta const* m = this; m != nullptr; m = m->base) {
            if (m == other) {
                return true;
            }
        }
        return false;
    }

    /// Adjust `ptr` (which points to a `this`-typed object) so that it points to
    /// the `target` base sub-object. Returns nullptr when unrelated.
    [[nodiscard]] void* castTo(void* ptr, ClassMeta const* target) const noexcept {
        if (ptr == nullptr || target == nullptr || !derivesFrom(target)) {
            return nullptr;
        }
        void* current = ptr;
        for (ClassMeta const* m = this; m != target; m = m->base) {
            if (m->upcast == nullptr || m->base == nullptr) {
                return nullptr;
            }
            current = m->upcast(current);
        }
        return current;
    }
};

/// Per-engine registry of every native class bound into that engine.
class ClassRegistry {
public:
    [[nodiscard]] ClassMeta* find(std::type_index type) {
        auto it = mClasses.find(type);
        return it == mClasses.end() ? nullptr : it->second.get();
    }
    [[nodiscard]] ClassMeta const* find(std::type_index type) const {
        auto it = mClasses.find(type);
        return it == mClasses.end() ? nullptr : it->second.get();
    }

    ClassMeta& add(std::unique_ptr<ClassMeta> meta) {
        ClassMeta* raw = meta.get();
        mClasses.emplace(raw->type, std::move(meta));
        return *raw;
    }

    template <typename... Args>
    ClassMeta& emplace(Args&&... args) {
        auto meta = std::make_unique<ClassMeta>(std::forward<Args>(args)...);
        return add(std::move(meta));
    }

    bool   empty() const { return mClasses.empty(); }
    size_t size() const { return mClasses.size(); }

    auto begin() { return mClasses.begin(); }
    auto end() { return mClasses.end(); }
    auto begin() const { return mClasses.begin(); }
    auto end() const { return mClasses.end(); }

private:
    std::unordered_map<std::type_index, std::unique_ptr<ClassMeta>> mClasses;
};

} // namespace ls::script
