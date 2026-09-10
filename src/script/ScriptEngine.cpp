#include "script/ScriptEngine.h"

#include <utility>

#include "script/TimerManager.h"

namespace ls::script {

ScriptEngine::ScriptEngine(std::string name, std::filesystem::path pluginDir, std::shared_ptr<ll::io::Logger> logger)
: mName(std::move(name)), mPluginDir(std::move(pluginDir)), mLogger(std::move(logger)),
  mTimers(std::make_unique<TimerManager>(this)) {}

ScriptEngine::~ScriptEngine() = default;

TimerManager& ScriptEngine::timers() const noexcept { return *mTimers; }

} // namespace ls::script
