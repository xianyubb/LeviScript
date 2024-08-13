#pragma once

#include <ll/api/mod/Manifest.h>
#include <ll/api/mod/ModManager.h>
#include <string_view>

namespace ls {

class ModManager : public ll::mod::ModManager {
public:
    ModManager();

    ModManager(const ModManager&)                          = delete;
    ModManager(ModManager&&)                               = delete;
    auto operator=(const ModManager&) -> ModManager&       = delete;
    auto operator=(ModManager&&) -> ModManager&            = delete;

    ~ModManager() override = default;

private:
    ll::Expected<> load(ll::mod::Manifest manifest) override;
    ll::Expected<> unload(std::string_view name) override;
};

} // namespace lse
