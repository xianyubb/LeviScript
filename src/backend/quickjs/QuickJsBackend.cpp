#include "backend/quickjs/QuickJsBackend.h"

#include <memory>
#include <string>
#include <utility>

#include "backend/quickjs/QuickJsEngine.h"
#include "ll/api/io/Logger.h"
#include "script/BackendRegistry.h"
#include "script/ScriptEngine.h"

namespace ls::backend::quickjs {

void registerBackend() {
    ls::script::BackendRegistry::getInstance().registerBackend(
        kBackendType,
        [](std::string const&                     pluginName,
           std::filesystem::path const&           rootDir,
           std::shared_ptr<ll::io::Logger> const& logger) -> std::shared_ptr<ls::script::ScriptEngine> {
            return std::make_shared<QuickJsEngine>(pluginName, rootDir, logger);
        }
    );
}

} // namespace ls::backend::quickjs
