#pragma once

#include <string_view>

namespace ls::baselib {

// The bootstrap library evaluated into every plugin engine right after the
// native APIs are bound and before the plugin entry runs. It is embedded as a
// string so a plugin needs no extra files on disk to work.
//
// It provides:
//   * CommonJS `ll.require(spec)` built on the native path/file helpers
//   * `console` aliased to the plugin logger
//   * `setImmediate` / `clearImmediate` on top of the native timers
inline constexpr std::string_view kBaseLibSource = R"LSBASE(
"use strict";
(function () {
    var moduleCache = Object.create(null);

    function loadModule(filename) {
        var cached = moduleCache[filename];
        if (cached) {
            return cached.exports;
        }
        var source = ll.__readFile(filename);
        if (source === null || source === undefined) {
            throw new Error("Cannot find module '" + filename + "'");
        }
        var dirname = ll.__dirname(filename);
        var module = { exports: {}, filename: filename, id: filename };
        moduleCache[filename] = module;

        var localRequire = function (spec) {
            var resolved = ll.__resolvePath(dirname, spec);
            if (!resolved) {
                throw new Error("Cannot resolve module '" + spec + "' from '" + filename + "'");
            }
            return loadModule(resolved);
        };

        var wrapper = new Function(
            "exports", "require", "module", "__filename", "__dirname", source
        );
        wrapper(module.exports, localRequire, module, filename, dirname);
        return module.exports;
    }

    ll.require = function (spec) {
        var base = ll.__pluginDir();
        var resolved = ll.__resolvePath(base, spec);
        if (!resolved) {
            throw new Error("Cannot resolve module '" + spec + "'");
        }
        return loadModule(resolved);
    };

    globalThis.setImmediate = function (callback) {
        var args = Array.prototype.slice.call(arguments, 1);
        return setTimeout(function () { callback.apply(null, args); }, 0);
    };
    globalThis.clearImmediate = function (id) { return clearTimeout(id); };

    globalThis.console = {
        log: function () { logger.info.apply(logger, arguments); },
        info: function () { logger.info.apply(logger, arguments); },
        warn: function () { logger.warn.apply(logger, arguments); },
        error: function () { logger.error.apply(logger, arguments); },
        debug: function () { logger.debug.apply(logger, arguments); },
        trace: function () { logger.trace.apply(logger, arguments); }
    };
})();
)LSBASE";

} // namespace ls::baselib
