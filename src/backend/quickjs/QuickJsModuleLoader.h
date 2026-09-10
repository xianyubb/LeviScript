#pragma once

namespace ls::backend::quickjs {

class QuickJsEngine;

/// Install the ES module loader on the engine's runtime.
///
/// Enables top level `import` / `export` and dynamic `import()`. Specifiers are
/// resolved with a Node.js flavoured algorithm:
///   * relative ("./a", "../b") -> against the importing module's directory
///   * absolute ("/a", "C:/a")  -> against the filesystem / plugin root
///   * bare     ("lodash")      -> every `node_modules` from the importer upwards,
///                                 then the plugin root
/// Files are probed with "", ".js", ".mjs", ".cjs", ".json" and, for directories,
/// "index.{js,mjs,cjs}".
void installModuleLoader(QuickJsEngine& engine);

} // namespace ls::backend::quickjs
