#include "mod/Mod.h"

#include "engine/EngineOwnData.h"
#include "mod/LeviScript.h"


#include <ll/api/mod/Manifest.h>

namespace ls {

Mod::Mod(const ll::mod::Manifest& manifest) : ll::mod::Mod(manifest) {}

std::shared_ptr<ll::mod::Mod> Mod::current() {
    return levi_script::getModManager().getMod(ENGINE_OWN_DATA()->modName);
}
} // namespace ls
