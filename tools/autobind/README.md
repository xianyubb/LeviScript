# autobind - header-driven binding generator

Turns LeviLamina / Minecraft C++ headers into binding code for the LeviScript
runtime binder. Two stages, so the intermediate spec can be inspected or edited:

```
headers --parse_headers.py (libclang)--> binding spec (JSON) --autobind.py--> GeneratedApi.cpp
```

`parse_headers.py` can also call `autobind.py` for you (`--cpp`), producing the
C++ in one shot.

## Requirements

- Python 3.9+
- `clang.cindex` (pip: `pip install libclang`)
- a libclang library. It is located from, in order: config `libclangPath`,
  the `LIBCLANG_PATH` env var, `C:/Program Files/LLVM/bin/libclang.dll`,
  then the pip `libclang` package.

## What it extracts

| C++ construct | Emitted binding |
| --- | --- |
| class / struct (public) | `ClassBinder::registerClass<T, Base>` (base resolved, inheritance kept) |
| member function | `ClassBinder::method<T>` (const aware; overloads get an explicit `static_cast`) |
| static member function | `ClassBinder::staticMethod<T>` |
| public data member | `ClassBinder::property<T>(..., &T::field)` |
| public constructor | `ClassBinder::constructor<T>` (script `new T(...)`) |
| free function | namespace function via `makeNativeFunction` |
| class template | bound for the instantiations listed in `templateClasses` |
| function template | bound for the instantiations listed in `templateFunctions` |

Only PUBLIC, non-operator declarations whose signature uses types the runtime
binder understands are emitted. Everything else is written to the skip report so
nothing silently fails to compile. Bindable types: `bool`, integers, floats,
enums, `std::string`, `std::optional<T>`, `std::vector<T>`, and pointers /
references / `shared_ptr` / `unique_ptr` to classes that are themselves being
bound (or listed in `knownNativeClasses`).

Integer/float *typedefs* (`uint16_t`, `size_t`, `int32_t`, ...) are resolved via
the canonical type, so they bind as `number`.

### Binding policy: skipped markers

Declarations prefixed with `LLNDAPI` (`[[nodiscard]]` API) or `MCNAPI`
(Minecraft native-binary symbols) are **never bound**; they are detected on the
declaration's own source line and recorded in the skip report. Plain `LLAPI`
(dllexport) declarations are bound normally. Adjust `SKIP_MACROS` in
`parse_headers.py` to change this policy.

## Config

```jsonc
{
  "libclangPath": "C:/Program Files/LLVM/bin/libclang.dll", // optional
  "std": "c++20",
  "scriptNamespace": "mc",              // script global that receives the API
  "functionName": "bindGeneratedMcApi", // emitted C++ function (default derived)
  "headers": ["path/to/Header.h"],      // parsed AND the only files bound from
  "includeDirs": [".../levilamina/include", ".../fmt/include"],
  "defines": ["LL_PLAT_S", "NOMINMAX"],
  "namespaces": ["mc", "ll"],           // only bind entities in these C++ namespaces
  "include": ["^Player"],               // optional name allow-list (regex)
  "exclude": ["^_"],                    // optional name deny-list (regex)
  "emitIncludes": ["mc/Player.h"],      // #include lines added to the generated file
  "knownNativeClasses": ["::mc::Block"],// treat these as already-bound native types
  "allowStringView": false,
  "closure": false,                    // auto-bind unfamiliar referenced classes (see below)
  "templateClasses": [
    { "templateOf": "::mc::AABB", "cpp": "::mc::AABB<float>", "scriptName": "AABBf" }
  ],
  "templateFunctions": [
    { "scriptName": "clampInt", "cpp": "&::mc::clamp<int>" }
  ]
}
```

`headers` is both the parse input and the filter: only declarations *located in
those files* are bound, so transitive includes (fmt, entt, mc internals, ...)
are not pulled in. Provide `includeDirs` / `defines` so the headers parse
cleanly; unresolved includes produce warnings but the target file's own
declarations are still extracted.

### Templates

A template cannot be bound generically at runtime - the binder needs concrete
types. List the instantiations you want:

- `templateClasses`: for each entry, the class template's members are re-emitted
  against the instantiated type (`&::mc::AABB<float>::method`). Because member
  signatures keep dependent types in the AST, template-instantiation members are
  emitted without the signature check (trust the config); overloaded template
  methods and parameterised constructors are skipped and reported.
- `templateFunctions`: give the explicit instantiation address (`&::mc::clamp<int>`)
  and the script name.

## Usage

```powershell
# stage 1 + 2 in one shot, plus a skip report, an expected-API file and a .d.ts
python parse_headers.py config/mc.json --cpp ../../../src/native/api/GeneratedMcApi.cpp `
    --report report.json --emit-expected expected_mc.json --dts mc.d.ts

# or stage by stage
python parse_headers.py config/mc.json --spec spec.json --report report.json
python autobind.py spec.json -o GeneratedMcApi.cpp --emit-expected expected_mc.json --dts mc.d.ts
```

`--dts` writes a TypeScript declaration for the bound namespace (C++ types mapped
to TS, `extends` preserved, raw pointers typed as `NativePointer`). Ship it (or the
hand-written `types/levi-script.d.ts` for the built-ins) with your plugin for
editor autocomplete.

Wire the generated function into the engine by calling it from
`ls::native::bindApis` (in `src/native/NativeModule.cpp`), then rebuild.

## Batch generation from real headers

`config/` holds one JSON config per real header (the `includeDirs` + `defines`
for the whole LeviLamina dependency graph are already filled in). `generate_all.py`
runs them all and writes **one binding file per header**, named after the header
path so each generated file corresponds to exactly one source header:

```powershell
python tools/autobind/generate_all.py
# config/ll_data_version.json  (ll/api/data/Version.h)
#   -> src/native/generated/ll_api_data_Version.cpp
#   -> types/generated/ll_api_data_Version.d.ts
#   -> config/ll_data_version.report.json
```

The generated `bindGenerated...` functions live in `ls::native::generated` and are
called from `bindApis` (`src/native/NativeModule.cpp`). Each **merges into the
matching hand-written namespace**, so `ll/api/data/Version.h` becomes `ll.Version`
alongside the built-in `ll.*` API (constructor + `major`/`minor`/`patch`/`build`
properties + `to_string()`/`isIdenticalTo()`). To bind another header: drop a new
config into `config/` (one header each), re-run `generate_all.py`, and call the
new function from `bindApis`.

> Real `mc/` headers (under `mc/deps/...`, `mc/world/...`) parse and bind with the
> same config - `BlockPos` is wired into the mod and verified on a real server.
> Coverage is still partial: complex signatures (`Expected<>`, `Generator<>`,
> `std::variant`, `string_view`) and fields hidden in CRTP bases are skipped and
> reported rather than silently dropped.

## Closure binding ("bind every unfamiliar class you meet")

Set `"closure": true` to make the generator bind the **transitive closure** of the
seed classes instead of skipping methods that reference not-yet-bound types:

- Starting from the seed (the `namespaces` / `include` selection), it scans every
  bound class's member signatures (return types, parameters, fields, bases) for
  referenced native classes and **binds those too**, recursing until no new class
  appears. Termination is guaranteed by a visited set (each class bound once); there
  are intentionally **no depth/count caps**.
- **Cross-header**: a referenced class that is only forward-declared in the parsed
  files is resolved by the LL/MC "header named after its class" convention
  (`Vec3` -> `Vec3.h`), which is then parsed and indexed on demand (`HeaderResolver`).
  If a definition cannot be located it is reported and left to the `NativePointer`
  fallback (pointers) or skipped (references).
- The generated file `#include`s the defining header of **every** bound class (not
  just the seed header), so cross-header types are complete at compile time.
- **Templates**: a concrete specialization encountered (e.g. `intN3<BlockPos>`,
  `floatN3<Vec3>`) is auto-bound as that instantiation (trust mode). Specializations
  that still carry *dependent* arguments (`intN3<BaseType>`,
  `FloatN<type-parameter-0-0, ...>`, `boolN<sizeof...(C)>`) are rejected - they are
  patterns, not concrete types. The generic template is never walked, so its
  dependent members do not leak in.
- `std::` types are never pulled in (they are not LL/MC API).

Value-type usage of a closure-bound class (returned/passed **by value**, e.g.
`BlockPos east()`) is marshalled by the binder's native by-value conversions
(`ToScript<T>` wraps an owned copy; `FromScript<T>` copies out of the wrapper).

Worked examples (both shipped in `config/`):

- `ll_data_version.json` - `ll/api/data/Version.h` with `closure` -> binds `Version`
  **plus** `PreRelease` and `detail::from_chars_result` (discovered from the
  `optional<PreRelease>` field and the `from_chars` return type). Wired into the mod
  and verified on a real server.
- `mc_blockpos.json` - `mc/world/level/BlockPos.h` with `closure` ->
  `1 seed -> 6 concrete classes`: `BlockPos`, `intN3<BlockPos>` (the CRTP base that
  holds x/y/z), `Vec3`, `floatN3<Vec3>`, `Vec2`, `floatN2<Vec2>` - `Vec3`/`Vec2`
  resolved cross-header, the three specializations auto-instantiated. **Wired into
  the mod and verified on a real server**: `mc.BlockPos.ZERO().toString()` ->
  `Pos(0,0,0)`, `MAX()` -> `Pos(2147483647,...)`, `zero.east().toString()` ->
  `Pos(1,0,0)` (a by-value return). `MCAPI` (MC-binary) symbols link and run through
  LeviLamina's bedrock symbol provider (`bedrock_runtime_api.lib`).

> Inheritance is reflected end to end: a template-specialization base is matched by
> its canonical spelling, so the codegen emits
> `registerClass<::BlockPos, ::ll::math::intN3<BlockPos>>`, the `.d.ts` says
> `class BlockPos extends intN3BlockPos`, and at runtime `zero instanceof
> mc.intN3BlockPos` is `true` (verified on a real server). Remaining gap: a CRTP
> base's *own members* are not emitted yet - `intN3<BlockPos>`'s x/y/z live in a
> deeper dependent base (`IntN<...>`), so the bound base class is currently empty;
> flattening inherited members is future work. Overloaded methods also collapse to
> the last emitted overload (JS has no overloading).

## Self test

`tests/sample_api.h` mimics the shape of a real LL header (inheritance,
overloads, statics, fields, a class template and a function template). Run:

```powershell
python parse_headers.py tests/sample_api.json --spec out_spec.json --cpp out.cpp --report out_report.json
```

Expected: classes `Base`, `Derived` (base `Base`), `IntBox`, `FloatBox`;
functions `add`, `greet`, `identityInt`; the two `compute` overloads emitted with
`static_cast`; `takesRawPtr` skipped (raw `int*` parameter).

`tests/macro_skip.h` checks the LLNDAPI/MCNAPI policy:

```powershell
python parse_headers.py tests/macro_skip.json --spec out.json --report out_report.json
```

Expected: `keepFunc` + `Keep` (`keepMethod`, `field`) bound (plain `LLAPI`);
`dropNodiscard`, `dropMcNative`, `Drop`, `Keep::dropMethodNd`, `Keep::dropMethodMc`
skipped (`LLNDAPI` / `MCNAPI`).

`tests/closure/` checks transitive closure binding across headers: seed `A`
(`tests/closure/A.h`) references `B` (pointer) and `C` (reference), and `C`
references `D`. With `"closure": true`:

```powershell
python parse_headers.py tests/closure.json --spec out.json   # -> closure: 1 seed -> 4 bound classes
```

Expected: `A`, `B`, `C`, `D` all bound (B.h/C.h/D.h auto-resolved), and A's
`getB`/`useC` and C's `getD` become bindable; nothing skipped.
