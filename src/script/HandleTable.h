#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>

namespace ls::script {

/// A minimal slot map that turns heavyweight backend value references into small
/// integer handles. Handle 0 is reserved as the invalid sentinel.
///
/// Backends instantiate it with their own slot type, e.g. QuickJS stores a
/// `JSValue` plus bookkeeping per slot.
template <typename Slot>
class HandleTable {
public:
    /// Insert `slot` and return a fresh, owning handle.
    uint32_t insert(Slot slot) {
        uint32_t id = mNext++;
        if (mNext == 0) {
            mNext = 1; // never hand out the invalid handle
        }
        mSlots.emplace(id, std::move(slot));
        return id;
    }

    /// Borrowed access to a slot; nullptr when the handle is unknown.
    [[nodiscard]] Slot* find(uint32_t id) {
        auto it = mSlots.find(id);
        return it == mSlots.end() ? nullptr : &it->second;
    }
    [[nodiscard]] Slot const* find(uint32_t id) const {
        auto it = mSlots.find(id);
        return it == mSlots.end() ? nullptr : &it->second;
    }

    bool contains(uint32_t id) const { return mSlots.find(id) != mSlots.end(); }

    /// Drop a slot without moving it out (used when releasing a handle).
    bool erase(uint32_t id) { return mSlots.erase(id) > 0; }

    /// Move a slot out of the table and erase it, transferring ownership to `out`.
    bool extract(uint32_t id, Slot& out) {
        auto it = mSlots.find(id);
        if (it == mSlots.end()) {
            return false;
        }
        out = std::move(it->second);
        mSlots.erase(it);
        return true;
    }

    void   clear() { mSlots.clear(); }
    size_t size() const { return mSlots.size(); }

private:
    std::unordered_map<uint32_t, Slot> mSlots;
    uint32_t                           mNext = 1;
};

} // namespace ls::script
