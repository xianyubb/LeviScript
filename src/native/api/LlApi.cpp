#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ll/api/io/FileUtils.h"
#include "ll/api/utils/StringUtils.h"
#include "native/NativeApi.h"
#include "native/ValueJson.h"
#include "script/EngineRegistry.h"
#include "script/EngineScope.h"
#include "script/Exception.h"
#include "script/Local.h"
#include "script/ScriptEngine.h"
#include "script/bind/Bind.h"

namespace fs = std::filesystem;

namespace ls::native {

using ls::script::EngineRegistry;
using ls::script::EngineScope;
using ls::script::getGlobal;
using ls::script::kInvalidHandle;
using ls::script::Local;
using ls::script::makeArray;
using ls::script::makeFunction;
using ls::script::makeNativeFunction;
using ls::script::makeObject;
using ls::script::makeString;
using ls::script::NativeFunction;
using ls::script::Object;
using ls::script::ScriptEngine;
using ls::script::ScriptException;
using ls::script::Value;
using ls::script::ValueHandle;

namespace {

constexpr int kVersionMajor = 0;
constexpr int kVersionMinor = 1;
constexpr int kVersionPatch = 0;

[[nodiscard]] std::string toU8(fs::path const& path) { return ll::string_utils::u8str2str(path.u8string()); }
[[nodiscard]] fs::path    fromU8(std::string const& str) { return ll::file_utils::u8path(str); }

[[nodiscard]] std::optional<fs::path> probeFile(fs::path const& candidate) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
        return candidate;
    }
    for (auto const* ext : {".js", ".mjs", ".cjs", ".json"}) {
        fs::path withExt = candidate;
        withExt         += ext;
        if (fs::exists(withExt, ec) && fs::is_regular_file(withExt, ec)) {
            return withExt;
        }
    }
    if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
        for (auto const* index : {"index.js", "index.mjs", "index.cjs"}) {
            fs::path candidateIndex = candidate / index;
            if (fs::exists(candidateIndex, ec) && fs::is_regular_file(candidateIndex, ec)) {
                return candidateIndex;
            }
        }
    }
    return std::nullopt;
}

/// Resolve a CommonJS specifier against a base directory (used by ll.require).
[[nodiscard]] std::optional<std::string> resolveRequirePath(std::string const& baseU8, std::string const& spec) {
    if (spec.empty()) {
        return std::nullopt;
    }
    fs::path base   = fromU8(baseU8);
    fs::path specPath(spec);
    fs::path target = specPath.is_absolute() ? specPath : (base / spec);
    target          = fs::absolute(target).lexically_normal();
    if (auto resolved = probeFile(target)) {
        return toU8(*resolved);
    }
    return std::nullopt;
}

/// Build the cross-plugin proxy for an exported function.
[[nodiscard]] NativeFunction makeImportProxy(std::string key) {
    return [key = std::move(key)](ScriptEngine& caller, ValueHandle, ValueHandle const* argv,
                                  int argc) -> ValueHandle {
        auto exported = EngineRegistry::getInstance().getExport(key);
        if (exported.owner == nullptr || exported.func == kInvalidHandle) {
            throw ScriptException("imported function not found: " + key);
        }
        if (exported.owner == &caller) {
            return caller.call(exported.func, kInvalidHandle, argv, argc);
        }
        // Different engine: bridge arguments and result through JSON.
        nlohmann::json         argsJson = nlohmann::json::array();
        for (int i = 0; i < argc; ++i) {
            argsJson.push_back(valueToJson(caller, argv[i]));
        }
        std::vector<ValueHandle> ownerArgs;
        ownerArgs.reserve(argsJson.size());
        for (auto const& element : argsJson) {
            ownerArgs.push_back(jsonToValue(*exported.owner, element));
        }
        ValueHandle ownerResult;
        {
            EngineScope scope(exported.owner);
            ownerResult = exported.owner->call(exported.func, kInvalidHandle, ownerArgs.data(),
                                               static_cast<int>(ownerArgs.size()));
        }
        nlohmann::json resultJson = valueToJson(*exported.owner, ownerResult);
        exported.owner->release(ownerResult);
        for (ValueHandle handle : ownerArgs) {
            exported.owner->release(handle);
        }
        return jsonToValue(caller, resultJson);
    };
}

} // namespace

void bindLlApi(ScriptEngine& engine) {
    Local<Object> global = getGlobal(engine);
    Local<Object> ll     = makeObject(engine);

    // ll.version() -> { major, minor, patch }
    ll.setProperty(
        "version",
        makeFunction(engine, makeNativeFunction([]() -> Local<Object> {
                         ScriptEngine& self   = *EngineScope::current();
                         Local<Object> result = makeObject(self);
                         result.setProperty("major", Local<Value>(&self, self.newInteger(kVersionMajor)));
                         result.setProperty("minor", Local<Value>(&self, self.newInteger(kVersionMinor)));
                         result.setProperty("patch", Local<Value>(&self, self.newInteger(kVersionPatch)));
                         return result;
                     }))
    );

    // ll.listPlugins() -> string[]
    ll.setProperty(
        "listPlugins",
        makeFunction(engine, makeNativeFunction([]() -> std::vector<std::string> {
                         return EngineRegistry::getInstance().listEngines();
                     }))
    );

    // ll.isDebugMode() -> bool
    ll.setProperty("isDebugMode", makeFunction(engine, makeNativeFunction([]() -> bool { return false; })));

    // ll.dumpApiSurface(path?) -> JSON string describing every bound global.
    // When `path` is given the JSON is also written to that file (relative to the
    // plugin directory) and the resolved path is returned; this is what
    // tools/apivalidator consumes to verify the runtime API against a spec.
    ll.setProperty(
        "dumpApiSurface",
        makeFunction(engine, makeNativeFunction([](std::optional<std::string> path) -> std::string {
                         ScriptEngine* self = EngineScope::current();
                         if (self == nullptr) {
                             return "{}";
                         }
                         std::string surface = exportApiSurface(*self);
                         if (path.has_value()) {
                             fs::path target = self->pluginDir() / *path;
                             std::error_code ec;
                             fs::create_directories(target.parent_path(), ec);
                             ll::file_utils::writeFile(target, surface, false);
                             return toU8(target);
                         }
                         return surface;
                     }))
    );

    // ll.exportFunc(func, name, namespace = "default") -> bool
    ll.setProperty(
        "exportFunc",
        makeFunction(engine, makeNativeFunction([](Local<Value> func, std::string name,
                                                   std::optional<std::string> ns) -> bool {
                         ScriptEngine* self = EngineScope::current();
                         if (self == nullptr || func.kind() != ls::script::ValueKind::Function || name.empty()) {
                             return false;
                         }
                         std::string key = (ns.value_or("default")) + "::" + name;
                         return EngineRegistry::getInstance().exportFunc(key, self, func.abandon());
                     }))
    );

    // ll.importFunc(name, namespace = "default") -> function | null
    ll.setProperty(
        "importFunc",
        makeFunction(engine, makeNativeFunction([](std::string name, std::optional<std::string> ns) -> Local<Value> {
                         ScriptEngine& self = *EngineScope::current();
                         std::string   key  = (ns.value_or("default")) + "::" + name;
                         if (!EngineRegistry::getInstance().hasExport(key)) {
                             return Local<Value>(&self, self.newNull());
                         }
                         return Local<Value>(&self, self.newFunction(makeImportProxy(std::move(key))));
                     }))
    );

    // -- helpers consumed by BaseLib.js to implement CommonJS ll.require -----
    // ll.__pluginDir() -> string
    ll.setProperty(
        "__pluginDir",
        makeFunction(engine, makeNativeFunction([]() -> std::string {
                         ScriptEngine* self = EngineScope::current();
                         return self ? toU8(self->pluginDir()) : std::string{};
                     }))
    );
    // ll.__resolvePath(baseDir, spec) -> string | null
    ll.setProperty(
        "__resolvePath",
        makeFunction(engine, makeNativeFunction([](std::string base, std::string spec) -> std::optional<std::string> {
                         return resolveRequirePath(base, spec);
                     }))
    );
    // ll.__dirname(path) -> string
    ll.setProperty(
        "__dirname",
        makeFunction(engine, makeNativeFunction([](std::string path) -> std::string {
                         return toU8(fromU8(path).parent_path());
                     }))
    );
    // ll.__readFile(path) -> string | null
    ll.setProperty(
        "__readFile",
        makeFunction(engine, makeNativeFunction([](std::string path) -> std::optional<std::string> {
                         return ll::file_utils::readFile(fromU8(path), false);
                     }))
    );

    global.setProperty("ll", ll);
}

} // namespace ls::native
