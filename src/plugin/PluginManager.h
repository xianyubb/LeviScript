#pragma once

#include <string>
#include <string_view>

#include "ll/api/mod/ModManager.h"

namespace ls::plugin {

/// The script plugin loader. It plugs into LeviLamina's own mod management system
/// exactly like LegacyScriptEngine does: it is a ll::mod::ModManager registered
/// under one manifest `type` per language backend (e.g. "lse-quickjs"), so LL
/// routes matching plugin manifests to it and handles ordering/dependencies.
///
/// One PluginManager instance is created per registered backend.
class PluginManager final : public ll::mod::ModManager {
public:
    explicit PluginManager(std::string type);
    ~PluginManager() override;

private:
    ll::Expected<> load(ll::mod::Manifest manifest) override;
    ll::Expected<> unload(std::string_view name) override;
};

} // namespace ls::plugin
