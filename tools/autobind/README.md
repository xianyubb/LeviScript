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

> Real `mc/` headers live under `mc/deps/...` with deep dependency trees and use
> many value-type / template / `Expected<>` signatures the binder cannot marshal;
> they parse with the same config but yield sparse coverage. `ll/api/` headers are
> the low-risk starting point (they already compile inside the mod).

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
