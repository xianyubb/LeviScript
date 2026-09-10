#include "backend/quickjs/QuickJsModuleLoader.h"

#include <quickjs.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "backend/quickjs/QuickJsEngine.h"
#include "ll/api/io/FileUtils.h"
#include "ll/api/utils/StringUtils.h"

namespace fs = std::filesystem;

namespace ls::backend::quickjs {

namespace {

[[nodiscard]] char* allocCString(JSContext* ctx, std::string const& str) {
    auto* buffer = static_cast<char*>(js_malloc(ctx, str.size() + 1));
    if (buffer == nullptr) {
        return nullptr;
    }
    std::memcpy(buffer, str.c_str(), str.size() + 1);
    return buffer;
}

[[nodiscard]] std::string pathToModuleName(fs::path const& path) {
    return ll::string_utils::u8str2str(path.u8string());
}

[[nodiscard]] fs::path moduleNameToPath(char const* name) { return ll::file_utils::u8path(name); }

[[nodiscard]] std::optional<fs::path> tryResolveFile(fs::path const& candidate) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
        return candidate;
    }
    static constexpr char const* extensions[] = {".js", ".mjs", ".cjs", ".json"};
    for (auto const* ext : extensions) {
        fs::path withExt = candidate;
        withExt         += ext;
        if (fs::exists(withExt, ec) && fs::is_regular_file(withExt, ec)) {
            return withExt;
        }
    }
    if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
        static constexpr char const* indexes[] = {"index.js", "index.mjs", "index.cjs"};
        for (auto const* index : indexes) {
            fs::path candidateIndex = candidate / index;
            if (fs::exists(candidateIndex, ec) && fs::is_regular_file(candidateIndex, ec)) {
                return candidateIndex;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<fs::path>
resolveFromNodeModules(fs::path startDir, std::string_view specifier, fs::path const& rootDir) {
    while (true) {
        if (auto resolved = tryResolveFile(startDir / "node_modules" / fs::path(specifier))) {
            return resolved;
        }
        if (startDir == rootDir) {
            break;
        }
        fs::path parent = startDir.parent_path();
        if (parent == startDir || parent.empty()) {
            break;
        }
        startDir = parent;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<fs::path>
resolveSpecifier(fs::path const& baseFile, std::string_view specifier, fs::path const& rootDir) {
    std::string spec(specifier);
    if (spec.empty()) {
        return std::nullopt;
    }

    fs::path baseDir = baseFile.has_parent_path() ? baseFile.parent_path() : rootDir;

    bool const isRelative = spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0 || spec == "." || spec == ".." ||
                            spec.rfind(".\\", 0) == 0 || spec.rfind("..\\", 0) == 0;
    fs::path specPath(spec);
    bool const isAbsolute = specPath.is_absolute() || spec.front() == '/';

    if (isRelative || isAbsolute) {
        fs::path target = (isAbsolute && !specPath.is_absolute()) ? (rootDir / spec) : fs::path(spec);
        if (isRelative) {
            target = baseDir / spec;
        }
        target = fs::absolute(target).lexically_normal();
        return tryResolveFile(target);
    }

    if (auto resolved = resolveFromNodeModules(baseDir, specifier, rootDir)) {
        return resolved;
    }
    return tryResolveFile(fs::absolute(rootDir / spec).lexically_normal());
}

char* moduleNormalize(JSContext* ctx, char const* baseName, char const* name, void* /*opaque*/) {
    auto* engine = QuickJsEngine::fromContext(ctx);
    if (engine == nullptr) {
        JS_ThrowReferenceError(ctx, "module normalize called without a bound engine");
        return nullptr;
    }
    fs::path baseFile   = moduleNameToPath(baseName);
    auto     resolvedOpt = resolveSpecifier(baseFile, name, engine->rootDir());
    if (!resolvedOpt) {
        JS_ThrowReferenceError(ctx, "cannot resolve module '%s' imported from '%s'", name, baseName);
        return nullptr;
    }
    return allocCString(ctx, pathToModuleName(*resolvedOpt));
}

JSModuleDef* moduleLoad(JSContext* ctx, char const* name, void* /*opaque*/) {
    fs::path filePath = moduleNameToPath(name);
    auto     content  = ll::file_utils::readFile(filePath, true);
    if (!content) {
        JS_ThrowReferenceError(ctx, "cannot load module '%s' (file not found or unreadable)", name);
        return nullptr;
    }
    JSValue value =
        JS_Eval(ctx, content->data(), content->size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(value)) {
        return nullptr; // exception already pending
    }
    // The compiled module is owned by the runtime; drop the extra reference JS_Eval
    // handed us after extracting the module definition pointer.
    auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(value));
    JS_FreeValue(ctx, value);
    return module;
}

} // namespace

void installModuleLoader(QuickJsEngine& engine) {
    JS_SetModuleLoaderFunc(engine.runtime(), &moduleNormalize, &moduleLoad, nullptr);
}

} // namespace ls::backend::quickjs
