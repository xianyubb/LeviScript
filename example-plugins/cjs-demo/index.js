// LeviScript CommonJS demo plugin (classic script entry).
//
// Demonstrates: a non-module entry, `ll.require` for local CommonJS modules,
// cross-plugin `ll.importFunc`, and classic lifecycle hooks (plain top-level
// functions become globals in a classic script).

"use strict";

const helper = ll.require("./helper.js");

logger.info("cjs-demo loading (classic script entry)");
logger.info("helper.greet('world') =", helper.greet("world"));
logger.info("helper.VERSION =", helper.VERSION);

// Import the function that esm-demo exported (cross-plugin RPC). cjs-demo
// declares a dependency on esm-demo so it is loaded first.
const multiply = ll.importFunc("multiply", "math");
if (multiply) {
    logger.info("imported math::multiply(6, 7) =", multiply(6, 7));
} else {
    logger.warn("math::multiply not available (is esm-demo loaded?)");
}

logger.info("loaded plugins:", ll.listPlugins().join(", "));

function onLoad() {
    logger.info("cjs-demo onLoad");
}

function onEnable() {
    logger.info("cjs-demo onEnable");
}

function onDisable() {
    logger.info("cjs-demo onDisable");
}
