# LeviScript

A script-engine loader for [LeviLamina](https://github.com/LiteLDev/LeviLamina): it lets you
write server plugins in JavaScript (QuickJS today) that call the LeviLamina / Minecraft API,
while plugins themselves are managed by **LeviLamina's own mod system**.

LeviScript is a clean-room re-implementation of the role
[LegacyScriptEngine (LSE)](https://github.com/LiteLDev/LegacyScriptEngine) plays, with two
important differences:

- **No ScriptX.** The engine abstraction (`src/script`) is written from scratch directly on top
  of the QuickJS C API, and is designed so other language backends (Node.js / Python / Lua) can be
  added under `src/backend/*` without touching the rest.
- **Real native class inheritance.** The binding framework exposes C++ inheritance hierarchies to
  scripts - prototype chaining, `instanceof` across the hierarchy, inherited methods/properties and
  correct pointer adjustment when a derived instance is passed where a base is expected. This is the
  piece ScriptX never provided.

It is **LSE-plugin compatible**: plugins declare `"type": "lse-quickjs"` in their `manifest.json`
(the same manager type LSE's QuickJS backend registers), live in `plugins/<name>/`, and use the
familiar `ll` / `logger` / `mc` globals.

---

## Layout

```
src/
  script/            backend-agnostic abstraction (the "ScriptX replacement")
    ScriptEngine.*    abstract engine interface (eval/module/values/classes)
    Local.h           Local<T> RAII value wrappers + factories
    HandleTable.h     slot map turning backend values into small integer handles
    EngineScope.h     per-thread "current engine"
    TimerManager.*    setTimeout/setInterval on top of LeviLamina's server executor
    ClassRegistry.h   ClassMeta: the inheritance graph (base link + pointer upcast)
    BackendRegistry.* manifest type -> engine factory
    EngineRegistry.*  live engines + cross-plugin exported functions
    bind/
      TypeConverter.h C++ <-> script value marshalling traits
      Bind.h          makeNativeFunction + ClassBinder (functions, classes, inheritance)
  backend/
    quickjs/          QuickJS implementation of ScriptEngine + ES module loader
  native/             the script-visible API
    NativeModule.*    binds every API group; exports the API surface for validation
    ValueJson.*       value <-> JSON bridge (cross-plugin RPC)
    api/              ll / logger / system(timers) / mc(sample, inheritance demo)
  plugin/             PluginManager (ll::mod::ModManager) + ScriptPlugin (ll::mod::Mod)
  mod/LeviScript.*    the mod entry: registers backends + one PluginManager per backend
  baselib/BaseLib.h   embedded bootstrap JS (CommonJS require, console, setImmediate)
tools/
  autobind/           header -> binding-spec -> C++ generator (libclang based)
  apivalidator/       checks the live API surface against an expected spec
example-plugins/      esm-demo (ES modules + inheritance) and cjs-demo (CommonJS)
```

---

## Building

Requires [xmake](https://xmake.io) and a recent LLVM/clang-cl (the LeviLamina mod toolchain).
QuickJS is pulled as the `quickjs-ng` xmake package automatically.

```powershell
xmake f -y -p windows -a x64 -m release
xmake
```

The packed mod lands in `bin/LeviScript`. Install it into your server's `plugins/` (or `mods/`)
directory like any other LeviLamina mod.

---

## Writing a plugin

A plugin is a directory under the server's `plugins/` folder containing a `manifest.json`:

```json
{
    "name": "my-plugin",
    "entry": "index.mjs",
    "version": "1.0.0",
    "type": "lse-quickjs",
    "author": "you",
    "description": "does things",
    "dependencies": [ { "name": "some-other-plugin" } ]
}
```

- `name` must match the plugin directory name.
- `type` selects the backend; `lse-quickjs` is the QuickJS backend.
- `entry` is run as an **ES module** when it ends in `.mjs`, or when a `package.json` in the plugin
  directory has `"type": "module"`; otherwise it runs as a **classic script**.

### ES modules

`import` / `export` and dynamic `import()` work out of the box. Specifiers resolve Node-style:
relative (`./a`, `../b`), absolute, or bare (`lodash`) searched through every `node_modules`
directory from the importer upwards, then the plugin root. Extensions `.js/.mjs/.cjs/.json` and
`index.*` are probed automatically. See `example-plugins/esm-demo`.

### CommonJS

Classic scripts can use `ll.require("./helper.js")` (a CommonJS loader implemented in the embedded
BaseLib). See `example-plugins/cjs-demo`.

### Lifecycle hooks

Define any of `onLoad`, `onEnable`, `onDisable`, `onUnload`. In a classic script a top-level
`function onEnable(){}` is enough; in a module assign `globalThis.onEnable = () => {...}`.

---

## Script API (built in)

| Global | Purpose |
| --- | --- |
| `logger` | `trace/debug/info/warn/error/fatal(...args)` bound to the plugin's LeviLamina logger |
| `log`, `logDebug`, `colorLog` | LSE-compatible logging shortcuts |
| `console` | `log/info/warn/error/debug/trace` aliased to `logger` |
| `setTimeout/setInterval/clearTimeout/clearInterval` | server-thread timers |
| `setImmediate/clearImmediate` | `setTimeout(fn, 0)` |
| `systemTimeMillis()` | wall clock ms |
| `ll.version()` | `{major, minor, patch}` |
| `ll.listPlugins()` | names of loaded script plugins |
| `ll.require(spec)` | CommonJS require |
| `ll.exportFunc(fn, name, ns?)` / `ll.importFunc(name, ns?)` | cross-plugin calls (JSON-bridged) |
| `ll.dumpApiSurface(path?)` | export the live API surface as JSON (validation tooling) |
| `NativePointer` | universal, smart-pointer-backed native memory access (see below) |
| `mc.*` | sample native namespace demonstrating the inheritance-capable binder |

The `mc` namespace binds a small `Entity <- LivingEntity <- Player` hierarchy purely as a working
example / test of the binder. Replace or extend it with generated bindings for real MC/LL types
(see the tools below).

### Inheritance example

```js
const p = new mc.Player();
p.setName("Steve");
p.health = 12.5;                       // accessor inherited from LivingEntity
mc.describe(p);                        // takes an Entity&: derived -> base pointer adjustment
p instanceof mc.Entity;                // true (prototype chain)
p.getTypeName();                       // "Player" (virtual dispatch to the most derived override)
const q = mc.Player.create("Alex");    // static factory returning an owned instance
```

### Universal pointer system (`NativePointer`)

Scripts can indirectly manipulate native memory through `NativePointer`, and the
binding generator exports **any raw pointer type** (`void*`, `int*`, a pointer to
an unbound struct, ...) as a `NativePointer`. It is built on smart pointers and
is leak-free by construction:

- `NativePointer.alloc(n)` returns an **owning** pointer whose buffer is held by a
  `std::shared_ptr<void>` with a real deleter - freed automatically when the last
  `NativePointer` referring to it is garbage collected.
- `add()/sub()` return a new pointer that **shares** the same owner, so derived
  pointers keep the base allocation alive; none of them leak.
- `NativePointer.fromAddress(hex)` / `NativePointer.of(obj)` / a raw pointer
  returned by a bound function are **borrowed**: destroying them never frees
  foreign memory (no double free, no leak).

```js
const buf = NativePointer.alloc(16);   // owned, zero-initialized, freed on GC
buf.setInt32(0, 0x12345678);
buf.setString(8, "hi");
buf.getInt32(0);                       // 0x12345678
buf.getString(8);                      // "hi"
const view = buf.add(4);               // shares ownership with buf
buf.address();                         // full 64-bit address as a hex string

const p = NativePointer.of(player);    // borrow a native object's memory
p.as(mc.Player).getName();             // reinterpret the address as a bound class
```

Reads/writes are bounds-checked whenever the extent is known (owned allocations),
and typed accessors cover int8..int64 / uint8..uint32 / float / double / string.

## TypeScript declarations

`types/levi-script.d.ts` declares the whole built-in API (timers, `logger`,
`console`, `ll`, `NativePointer`, the `mc` sample hierarchy and the lifecycle
hooks) so plugin authors get autocomplete and type checking. Reference it from
your plugin's `tsconfig.json`.

Generated bindings can emit their own declarations too: `parse_headers.py --dts`
(or `autobind.py --dts`) writes a `.d.ts` for the bound namespace, mapping C++
types to TS (`int`->`number`, `std::string`->`string`, `std::vector<T>`->`T[]`,
`std::optional<T>`->`T | null`, native classes to their script name, and any other
raw pointer to `NativePointer`), preserving `extends` for inheritance.

---

## Binding native C++ (auto-binding tools)

The runtime binder lives in `src/script/bind/Bind.h` and is fully generic:

```cpp
LS_NATIVE_CLASS(::mc::Player)                                   // opt the type in

ClassBinder::registerClass<::mc::Player, ::mc::Entity>(engine, "Player");   // base first
ClassBinder::method<::mc::Player>(engine, "kick", &::mc::Player::kick);     // auto-marshalled
ClassBinder::property<::mc::Player>(engine, "health", &::mc::Player::health,
                                                       &::mc::Player::setHealth);
ClassBinder::property<::mc::Player>(engine, "level", &::mc::Player::mLevel); // data member
ClassBinder::constructor<::mc::Player>(engine, +[]() -> ::mc::Player* { return new ::mc::Player(); });
ClassBinder::expose<::mc::Player>(engine, mc.handle(), "Player");

// free functions / lambdas bind with zero glue:
ns.setProperty("add", makeFunction(engine, makeNativeFunction(+[](int a, int b){ return a + b; })));
```

Argument and return marshalling is automatic for `bool`, integers, floats, enums, `std::string`,
`std::optional<T>`, `std::vector<T>`, native class `T*` / `T&` / `std::shared_ptr<T>` /
`std::unique_ptr<T>`, and pass-through `Local<T>` / handles.

Hand-writing that for hundreds of MC symbols is impractical, so `tools/autobind` reads the headers
and generates it. See [`tools/autobind/README.md`](tools/autobind/README.md):

```
headers --(parse_headers.py, libclang)--> binding spec (JSON) --(autobind.py)--> GeneratedApi.cpp
```

`tools/apivalidator` then checks the *live* API against a spec. See
[`tools/apivalidator/README.md`](tools/apivalidator/README.md).

---

## Adding a new language backend

1. Create `src/backend/<lang>/` with a class implementing `ls::script::ScriptEngine`.
2. Provide a factory and register it: `BackendRegistry::getInstance().registerBackend("<type>", factory)`.
3. Call that registration from `LeviScript::load()`.

Everything above the backend (plugin loading, timers, the binding framework, native APIs) is
backend-agnostic and reused as-is. Plugins opt in with `"type": "<type>"` in their manifest.

---

## Notes & limitations

- All engine work happens on the server thread; timers are dispatched through
  `ll::thread::ServerThreadExecutor`.
- Cross-plugin `exportFunc/importFunc` bridges arguments through JSON, so it carries data
  (primitives/strings/arrays/plain objects), not live native objects.
- The class binder supports single-inheritance pointer adjustment; that covers the MC/LL type
  graph. `std::string_view` / `const char*` *parameters* are intentionally not auto-bound (lifetime
  hazards) - use `std::string`.
- Base classes must be registered before derived ones (the generator orders them for you).

## License

See [LICENSE](LICENSE).
