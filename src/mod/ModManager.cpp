#include "mod/ModManager.h"
#include "engine/EngineManager.h"
#include "engine/EngineOwnData.h"
#include "mod/LeviScript.h"
#include "mod/Mod.h"
// #include "mod/"
#include "API/EventAPI/EventAPI.h"
#include "utils/UsingScriptX.h"


#include <exception>
#include <memory>

#include <fmt/format.h>


#include "ll/api/Expected.h"
#include <ll/api/Logger.h>
#include <ll/api/io/FileUtils.h>
#include <ll/api/mod/Mod.h>
#include <ll/api/mod/ModManager.h>
#include <ll/api/service/ServerInfo.h>
#include <ll/api/utils/StringUtils.h>


constexpr auto ModManagerName = "ls-quickjs";

// Do not use legacy headers directly, otherwise there will be tons of errors.
void BindAPI(ScriptEngine* engine);
// void LLSERemoveTimeTaskData(script::ScriptEngine* engine);
// auto LLSERemoveAllEventListeners(script::ScriptEngine* engine) -> bool;
// auto LLSERemoveCmdRegister(script::ScriptEngine* engine) -> bool;
// auto LLSERemoveCmdCallback(script::ScriptEngine* engine) -> bool;
// auto LLSERemoveAllExportedFuncs(script::ScriptEngine* engine) -> bool;

namespace ls {

ModManager::ModManager() : ll::mod::ModManager(ModManagerName) {}

ll::Expected<> ModManager::load(ll::mod::Manifest manifest) {
    auto& logger = levi_script::LeviScript::getInstance().getSelf().getLogger();

    logger.info("加载模组 {}", manifest.name);

    if (hasMod(manifest.name)) {
        return ll::makeStringError("模组已经加载");
    }

    auto scriptEngine = EngineManager::newEngine(manifest.name);
    auto mod          = std::make_shared<Mod>(manifest);

    try {
        script::EngineScope engineScope(scriptEngine);

        // Set mods's logger title
        ENGINE_OWN_DATA()->logger.title = manifest.name;
        ENGINE_OWN_DATA()->modName      = manifest.name;

        BindAPI(scriptEngine);

#ifdef SCRIPT_ENGINE_BACKEND_QUICKJS
        // Try loadFile
        auto modDir                         = std::filesystem::canonical(ll::mod::getModsRoot() / manifest.name);
        auto entryPath                      = modDir / manifest.entry;
        ENGINE_OWN_DATA()->modFileOrDirPath = ll::string_utils::u8str2str(entryPath.u8string());
        try {
            scriptEngine->loadFile(entryPath.u8string());
        } catch (const script::Exception&) {
            // loadFile failed, try eval
            auto modEntryContent = ll::file_utils::readFile(entryPath);
            if (!modEntryContent) {
                return ll::makeStringError(fmt::format("读取模组入口错误: {}", entryPath.string()));
            }
            scriptEngine->eval(modEntryContent.value(), entryPath.u8string());
        }
        if (ll::getServerStatus() == ll::ServerStatus::Running) { // Is hot load
            // LLSECallEventsOnHotLoad(&scriptEngine);
        }
        ExitEngineScope exit;

#endif
        mod->onLoad([](ll::mod::Mod&) { return true; });
        mod->onUnload([](ll::mod::Mod&) { return true; });
        mod->onEnable([](ll::mod::Mod&) { return true; });
        mod->onDisable([](ll::mod::Mod&) { return true; });
    } catch (const Exception& e) {
        EngineScope engineScope(scriptEngine);
        auto        error = ll::makeStringError(
            fmt::v10::format("加载模组 {} 失败 信息: {}\n{}", manifest.name, e.message(), e.stacktrace())
        );
        ExitEngineScope exit;
        // LLSERemoveTimeTaskData(&scriptEngine);
        // LLSERemoveAllEventListeners(&scriptEngine);
        // LLSERemoveCmdRegister(&scriptEngine);
        // LLSERemoveCmdCallback(&scriptEngine);
        // LLSERemoveAllExportedFuncs(&scriptEngine);

        scriptEngine->getData().reset();

        EngineManager::unregisterEngine(scriptEngine);

        return error;
    }

    addMod(manifest.name, mod);

    return {};
}

ll::Expected<> ModManager::unload(std::string_view name) {
    auto& logger = levi_script::LeviScript::getInstance().getSelf().getLogger();

    try {
        // auto mod = std::static_pointer_cast<Mod>(getMod(name));

        // if (mod == nullptr) {
        //     return {};
        // }

        logger.info("卸载模组 {}", name);

        auto scriptEngine = EngineManager::getEngine(std::string(name));

        scriptEngine->getData().reset();

        EngineManager::unregisterEngine(scriptEngine);

#ifdef SCRIPT_ENGINE_BACKEND_QUICKJS
        scriptEngine->destroy();
#endif
        eraseMod(name); // TODO: handle unload errors

        return {};
    } catch (const std::exception& e) {
        return ll::makeStringError(fmt::v10::format("卸载模组 {} 失败 信息: {}", name, e.what()));
    }
}

} // namespace ls
