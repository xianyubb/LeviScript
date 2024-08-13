#pragma once

#include "mod/ModManager.h"

#include <ll/api/mod/Manifest.h>
#include <ll/api/mod/Mod.h>

namespace ls {

class Mod : public ll::mod::Mod {
    friend ModManager;

public:
    Mod(const ll::mod::Manifest& manifest);

    static std::shared_ptr<ll::mod::Mod> current();
};
} // namespace lse
