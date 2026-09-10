#include "script/BackendRegistry.h"

#include <utility>

namespace ls::script {

BackendRegistry& BackendRegistry::getInstance() {
    // Immortal singleton (see EngineRegistry): safe to reach during static teardown.
    static BackendRegistry* instance = new BackendRegistry();
    return *instance;
}

bool BackendRegistry::registerBackend(std::string type, EngineFactory factory) {
    if (type.empty() || !factory) {
        return false;
    }
    return mFactories.insert_or_assign(std::move(type), std::move(factory)).second;
}

bool BackendRegistry::hasBackend(std::string_view type) const {
    return mFactories.find(std::string(type)) != mFactories.end();
}

EngineFactory const* BackendRegistry::find(std::string_view type) const {
    auto it = mFactories.find(std::string(type));
    return it == mFactories.end() ? nullptr : &it->second;
}

std::vector<std::string> BackendRegistry::listBackends() const {
    std::vector<std::string> result;
    result.reserve(mFactories.size());
    for (auto const& [type, factory] : mFactories) {
        result.push_back(type);
    }
    return result;
}

} // namespace ls::script
