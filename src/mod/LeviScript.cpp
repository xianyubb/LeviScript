
#include <memory>


#include "mod/LeviScript.h"
#include "engine/GlobalShareData.h"
#include "engine/LocalShareData.h"
#include "mod/Mod.h"
#include "mod/ModManager.h"


#include "ScriptX/ScriptX.h"


#include "ll/api/mod/ModManager.h"
#include "ll/api/mod/ModManagerRegistry.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"



using namespace script;

std::shared_ptr<script::ScriptEngine> createEngine() {
#if !defined(SCRIPTX_BACKEND_WEBASSEMBLY)
    return std::shared_ptr<script::ScriptEngine>{
        new script::ScriptEngineImpl(nullptr),
        script::ScriptEngine::Deleter()
    };
#else
    return std::shared_ptr<script::ScriptEngine>{script::ScriptEngineImpl::instance(), [](void*) {}};
#endif
}

namespace levi_script {

namespace {

std::shared_ptr<ls::ModManager> ModManager;

void initializeLegacyStuff() {
    InitLocalShareData();
    InitGlobalShareData();
    // InitSafeGuardRecord();
    // EconomySystem::init();
#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_PYTHON
    // This fix is used for Python3.10's bug:
    // The thread will freeze when creating a new engine while another thread is blocking to read stdin
    // Side effects: sys.stdin cannot be used after this patch.
    // More info to see: https://github.com/python/cpython/issues/83526
    //
    // Attention! When CPython is upgraded, this fix must be re-adapted or removed!!
    //
    // PythonHelper::FixPython310Stdin::patchPython310CreateStdio();
    PythonHelper::initPythonRuntime();
#endif

    // InitBasicEventListeners();
    // InitMessageSystem();
    // MoreGlobal::Init();
}
};

static std::unique_ptr<LeviScript> instance;

LeviScript& LeviScript::getInstance() { return *instance; }

bool LeviScript::load() {
    getSelf().getLogger().debug("加载中...");
    // Code for loading the mod goes here.

    auto modManager = std::make_shared<ls::ModManager>();

    auto& ModManagerRegistry = ll::mod::ModManagerRegistry::getInstance();

    const auto& a = ModManagerRegistry.addManager(modManager);

    initializeLegacyStuff();
    return true;
}


bool LeviScript::enable() {

    getSelf().getLogger().debug("启用中...");
    // Code for enabling the mod goes here.
    return true;
}

bool LeviScript::disable() {
    getSelf().getLogger().debug("卸载中...");
    // Code for disabling the mod goes here.
    return true;
}

auto getModManager() -> ls::ModManager& {
    if (!ModManager) {
        throw std::runtime_error("ModManager is null");
    }
    return *ModManager;
}
} // namespace levi_script

LL_REGISTER_MOD(levi_script::LeviScript, levi_script::instance);
