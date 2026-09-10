# apivalidator - verify the live API against a spec

Checks that the API actually present in a running engine matches what you expect.
It is a recursive *subset* test: everything listed in the expectation must exist
in the dumped surface with a compatible shape; extra entries are allowed. Exit
code is non-zero when anything required is missing or mistyped, so it can gate CI.

## Workflow

1. **Dump the live surface.** Copy `dump_plugin/` into the server's `plugins/`
   directory and start the server once. Its `index.js` calls
   `ll.dumpApiSurface("api_surface.json")`, which writes the surface next to the
   plugin. Copy that file out.

2. **Produce an expectation.** Either write one by hand, or derive it from a
   binding spec with the generator:

   ```powershell
   python ../autobind/autobind.py spec.json -o out.cpp --emit-expected expected.json
   ```

   A ready-made expectation for the built-in API ships in `expected/builtin_api.json`.

3. **Validate.**

   ```powershell
   python validate.py --surface api_surface.json --expected expected/builtin_api.json --verbose
   ```

## Node shape

Both the dumped surface and the expectation use the same node shape (produced by
`ls::native::exportApiSurface`):

```jsonc
{ "type": "function" }
{ "type": "class", "methods": ["a", "b"], "staticMethods": ["create"] }
{ "type": "object", "members": { "child": { ... } } }
{ "type": "number" | "string" | "boolean" | "array" | "native" | ... }
```

- `methods` are the function-valued own properties of a class prototype.
- `staticMethods` are the function-valued own properties of the constructor
  object (what `ClassBinder::staticMethod` attaches).
- Accessor properties (`ClassBinder::property`) are not listed as methods; they
  surface as their current value type, so validate them as presence of the class
  rather than as methods.

## What it does and does not check

The validator confirms the **static shape** of the API (namespaces, classes,
functions, method names). Inheritance *behaviour* - `instanceof` across the
hierarchy, inherited methods resolving, virtual dispatch, base-pointer argument
adjustment - is exercised at runtime by `example-plugins/esm-demo`, which asserts
those cases through the `mc` sample hierarchy.
