#!/usr/bin/env python3
"""Export a header tree to mirrored per-header binding files (memory-bounded).

Policy (allExceptSkip): every function / method / constructor is exported EXCEPT those
carrying a skip macro (default MCNAPI). Client-marked declarations (default LLCAPI)
route to the client tree; everything else to the server tree. Data members are not
exported (functions/methods only).

Two phases keep memory bounded (only one heavy translation unit is alive at a time):

  Phase A (scan): parse each seed header once and record string-only facts - the types
    it defines, the native types its members reference, and its base edges. The TU is
    discarded immediately. This yields a global type registry plus the set of external
    (non-seed, e.g. Minecraft) types that must exist for the seed API to bind.

  Phase B (emit): parse each seed header again and build its full binding spec against
    the GLOBAL type set, so a member referencing a type defined in another header (or an
    external MC type) still marshals. Cross-file references and bases are wired with an
    #include of the owner header + an LS_NATIVE_CLASS here, and the registrar binds owner
    files first.

External types are bound OPAQUE (registerClass + expose + LS_NATIVE_CLASS + #include of
their defining header, resolved by the `<Leaf>.h` filename convention) in a mirrored file
for that header. Their members are NOT parsed, so heavy MC headers are never opened - this
is what keeps the export memory-bounded while still letting every ll signature bind.

Usage:
    python export_ll_tree.py tree_config.json [--report out.json]
"""

from __future__ import annotations

import argparse
import heapq
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional, Set, Tuple

from clang import cindex
from clang.cindex import CursorKind

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autobind             # noqa: E402
import parse_headers as ph  # noqa: E402


# --------------------------------------------------------------------------- #
# naming / layout
# --------------------------------------------------------------------------- #
def derive_names(header_rel: str, target: str) -> Tuple[str, str, str]:
    """(functionName, scriptNamespace, outputRelativePath) for an include-relative header
    path. Mirrors the source tree; namespace comes from the directory:
    `ll/api/io/Logger.h -> ll.io`, `mc/world/actor/Actor.h -> mc.world.actor`."""
    no_ext = header_rel
    for ext in (".hpp", ".h"):
        if no_ext.endswith(ext):
            no_ext = no_ext[:-len(ext)]
            break
    stem = re.sub(r"[^A-Za-z0-9]", "_", no_ext)
    func_name = ("bind_" if target == "server" else "bind_client_") + stem
    dirs = no_ext.split("/")[:-1]
    if dirs[:2] == ["ll", "api"]:
        ns_parts = ["ll"] + dirs[2:]
    else:
        ns_parts = dirs[:]
    namespace = ".".join(ns_parts) if ns_parts else "global"
    return func_name, namespace, no_ext + ".cpp"


def enumerate_headers(root: str) -> List[str]:
    out: List[str] = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.endswith((".h", ".hpp")):
                out.append(os.path.join(dirpath, name))
    return sorted(out)


def _leaf_of(canon_key: str) -> str:
    """`mc::Foo<mc::Bar>` -> `Foo`; `Actor` -> `Actor` (outermost name, no template args)."""
    base = canon_key.split("<", 1)[0]
    return base.split("::")[-1].strip()


class TemplateResolver:
    """Resolves a CLASS_TEMPLATE pattern by parsing its (small) defining header on demand
    and caching the result. Used to recover the dependent base of a template
    specialization base (e.g. `Cancellable<T> : T`) without walking heavy MC TUs."""

    def __init__(self, index, args: List[str], resolver: ph.HeaderResolver,
                 include_dirs: List[str], roots: List[str]):
        self.index = index
        self.args = args
        self.resolver = resolver
        self.include_dirs = include_dirs
        self.roots = set(roots)
        self.cache: Dict[str, Any] = {}
        self.tus: Dict[str, Any] = {}

    def __call__(self, qname: str, leaf: str):
        if qname in self.cache:
            return self.cache[qname]
        result = None
        for path in self.resolver.candidates(leaf):
            rel = ph.include_relative(path, self.include_dirs)
            if rel is None or rel.split("/")[0] not in self.roots:
                continue
            tu = self.tus.get(path)
            if tu is None:
                tu = ph.parse_one(self.index, path, self.args)
                self.tus[path] = tu
            result = self._find(tu.cursor, qname)
            if result is not None:
                break
        self.cache[qname] = result
        return result

    @staticmethod
    def _find(cursor, qname: str):
        for child in cursor.get_children():
            if child.kind == CursorKind.CLASS_TEMPLATE and ph.qualified_name(child) == qname \
                    and child.is_definition():
                return child
            if child.kind in (CursorKind.NAMESPACE, CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL,
                              CursorKind.CLASS_TEMPLATE):
                found = TemplateResolver._find(child, qname)
                if found is not None:
                    return found
        return None


# --------------------------------------------------------------------------- #
# Phase A: lightweight global scan (one TU at a time)
# --------------------------------------------------------------------------- #
def scan_headers(index, seed_headers: List[str], include_dirs: List[str], args: List[str],
                 base_cfg: Dict[str, Any], tmpl_resolver) -> Tuple[Dict[str, Dict[str, str]], Set[str], Set[str], Set[str]]:
    """Return (ll_types, refs, base_keep, skip_headers).

    ll_types : canonKey -> {cppType, scriptName, header} for every non-template class
               defined in a seed header.
    refs     : canonKeys of all native types referenced by seed members/bases.
    base_keep: canonKeys that appear as someone's base (must survive pruning).
    """
    ll_types: Dict[str, Dict[str, str]] = {}
    refs: Set[str] = set()
    base_keep: Set[str] = set()
    skip_headers: Set[str] = set()

    cfg = dict(base_cfg)
    cfg.update({"namespaces": None, "include": None, "apiTarget": "server",
                "exportPolicy": "allExceptSkip"})

    for i, header in enumerate(seed_headers, 1):
        header_rel = ph.include_relative(header, include_dirs)
        if header_rel is None:
            continue
        tu = ph.parse_one(index, header, args)
        if any(d.severity >= cindex.Diagnostic.Error for d in tu.diagnostics):
            skip_headers.add(header_rel)
            del tu
            continue
        target_files = {os.path.normcase(os.path.abspath(header))}
        extractor = ph.Extractor(cfg, target_files)
        extractor.run(tu)
        extractor.expand_base_instantiations(tmpl_resolver)
        ph.resolve_bases(extractor)
        for entry in extractor.classes.values():
            base_cpp = entry.get("_baseCppType")
            if base_cpp:
                base_keep.add(base_cpp.lstrip(":"))
            if entry.get("_templateInstantiation"):
                continue  # instantiations are bound locally in the derived file
            canon = entry.get("_canonKey") or entry["cppType"].lstrip(":")
            ll_types.setdefault(canon, {
                "cppType": entry["cppType"], "scriptName": entry["scriptName"],
                "header": entry.get("_header") or header_rel,
            })
            cursor = entry.get("_cursor")
            if cursor is not None:
                for ref in ph.referenced_classes(cursor):
                    refs.add(ref.cpp_type.lstrip(":"))
        for fn in extractor.functions:
            for ref in ph.referenced_by_function(fn["_cursor"]):
                refs.add(ref.cpp_type.lstrip(":"))
        del extractor, tu
        if i % 25 == 0:
            print(f"  scanned {i}/{len(seed_headers)} headers ({len(ll_types)} ll types, {len(refs)} refs)")
    print(f"  scan done: {len(ll_types)} ll types, {len(refs)} referenced types, "
          f"{len(base_keep)} base edges, {len(skip_headers)} unclean headers")
    return ll_types, refs, base_keep, skip_headers


def resolve_external(refs: Set[str], ll_types: Dict[str, Dict[str, str]], resolver: ph.HeaderResolver,
                     include_dirs: List[str], closure_roots: List[str]) -> Dict[str, Dict[str, str]]:
    """Map each referenced-but-not-ll type to a defining header under a closure root, so it
    can be bound opaque. Resolution is by the `<Leaf>.h` filename convention (no parsing)."""
    external: Dict[str, Dict[str, str]] = {}
    root_set = set(closure_roots)
    for canon in sorted(refs):
        if canon in ll_types:
            continue
        if canon.startswith("std::") or "::std::" in canon:
            continue
        leaf = _leaf_of(canon)
        if not leaf or not re.match(r"^[A-Za-z_]", leaf):
            continue
        cands: List[str] = []
        for path in resolver.candidates(leaf):
            rel = ph.include_relative(path, include_dirs)
            if rel and rel.split("/")[0] in root_set:
                cands.append(rel)
        # Only bind when the leaf resolves to EXACTLY ONE header under the roots. An
        # ambiguous leaf (e.g. "Impl", a common PIMPL nested-class name) would guess a
        # wrong header and emit a non-compiling opaque binding, so skip it instead; the
        # referencing member is then dropped by classify (type not in the bound set).
        if len(cands) != 1:
            continue
        chosen = cands[0]
        script = re.sub(r"[^A-Za-z0-9]", "", canon.split("::")[-1].rstrip(">")) or leaf
        external[canon] = {"cppType": "::" + canon if not canon.startswith("::") else canon,
                           "scriptName": script, "header": chosen}
    return external


# --------------------------------------------------------------------------- #
# cross-file wiring
# --------------------------------------------------------------------------- #
def wire_cross_file(spec: Dict[str, Any], owners: Dict[str, Dict[str, str]],
                    deps: Dict[str, Set[str]]) -> None:
    """Set baseCppType for cross-file bases and add LS_NATIVE_CLASS + #include + ordering
    deps for every base/member type owned by another file."""
    this_func = spec["functionName"]
    own: Set[str] = set()
    for cls in spec["classes"]:
        own.add(cls["cppType"].lstrip(":"))
        if cls.get("_canonKey"):
            own.add(cls["_canonKey"])

    def wire(ref: str) -> None:
        if not ref:
            return
        key = ref.lstrip(":")
        if key in own:
            return
        owner = owners.get(key)
        if owner is None or owner["bindFunc"] == this_func:
            return
        cpp = key if key.startswith("::") else "::" + key
        if cpp not in spec["extraNativeClasses"]:
            spec["extraNativeClasses"].append(cpp)
        if owner["header_rel"] and owner["header_rel"] not in spec["includes"]:
            spec["includes"].append(owner["header_rel"])
        deps.setdefault(this_func, set()).add(owner["bindFunc"])

    for cls in spec["classes"]:
        base_cpp = cls.get("_baseCppType")
        if base_cpp and base_cpp.lstrip(":") not in own:
            cls["base"] = None
            owner = owners.get(base_cpp.lstrip(":"))
            if owner is not None and owner["bindFunc"] != this_func:
                cls["baseCppType"] = base_cpp if base_cpp.startswith("::") else "::" + base_cpp
            # else: base is not bound anywhere -> drop the edge (base stays None) so we
            # never emit registerClass<Derived, Unbound> (which would fail the
            # is_native_class static_assert). The base is still in _refs; wire() adds it
            # only when an owner exists.
        elif not base_cpp:
            cls["base"] = None
        for ref in cls.get("_refs", []):
            wire(ref)
    for fn in spec["functions"]:
        for ref in fn.get("_refs", []):
            wire(ref)


def topo_sort_registrar(registrar: List[Tuple[str, str]], deps: Dict[str, Set[str]]) -> List[Tuple[str, str]]:
    """Order bind functions so every owner/base file binds before its dependents. Stable:
    ties keep enumeration order; cycles fall back to it too."""
    position = {func: i for i, (func, _) in enumerate(registrar)}
    indegree = {func: 0 for func, _ in registrar}
    adjacency: Dict[str, List[str]] = {func: [] for func, _ in registrar}
    for func, _ in registrar:
        for dep in deps.get(func, ()):
            if dep in indegree and dep != func:
                adjacency[dep].append(func)
                indegree[func] += 1
    ready = [position[func] for func, _ in registrar if indegree[func] == 0]
    heapq.heapify(ready)
    ordered: List[Tuple[str, str]] = []
    seen: Set[str] = set()
    while ready:
        item = registrar[heapq.heappop(ready)]
        if item[0] in seen:
            continue
        seen.add(item[0])
        ordered.append(item)
        for nxt in adjacency[item[0]]:
            indegree[nxt] -= 1
            if indegree[nxt] == 0:
                heapq.heappush(ready, position[nxt])
    if len(ordered) < len(registrar):
        for item in registrar:
            if item[0] not in seen:
                ordered.append(item)
                seen.add(item[0])
    return ordered


def emit_registrar(output_root: str, prefix: str, registrar: List[Tuple[str, str]], entry_name: str) -> str:
    ll_dir = os.path.join(output_root, prefix)
    os.makedirs(ll_dir, exist_ok=True)
    lines = [
        "// AUTO-GENERATED by tools/autobind/export_ll_tree.py - DO NOT EDIT.",
        "// Declares and calls every per-header binding function for this exported tree,",
        "// ordered so base classes / referenced types are registered before their users.",
        "",
        "#include \"script/ScriptEngine.h\"",
        "",
        "namespace ls::native::generated {",
        "",
    ]
    for func_name, _ in registrar:
        lines.append(f"void {func_name}(::ls::script::ScriptEngine& engine);")
    lines += ["", f"void {entry_name}(::ls::script::ScriptEngine& engine) {{"]
    for func_name, _ in registrar:
        lines.append(f"    {func_name}(engine);")
    lines += ["}", "", "} // namespace ls::native::generated", ""]
    path = os.path.join(ll_dir, "_registrar.cpp")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))
    return path


def write_spec(out_root: str, out_rel: str, spec: Dict[str, Any]) -> str:
    out_path = os.path.join(out_root, out_rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(autobind.generate_cpp(spec))
    return out_path


def clean_stale(out_root: str, written: Set[str]) -> int:
    """Delete .cpp files under out_root that were NOT written this run, so bindings for
    headers/types that are no longer exported (e.g. a now-skipped specialization) do not
    linger and break the build."""
    removed = 0
    for dirpath, _dirs, filenames in os.walk(out_root):
        for name in filenames:
            if not name.endswith(".cpp"):
                continue
            path = os.path.normcase(os.path.abspath(os.path.join(dirpath, name)))
            if path not in written:
                try:
                    os.remove(path)
                    removed += 1
                except OSError:
                    pass
    return removed


# --------------------------------------------------------------------------- #
# driver
# --------------------------------------------------------------------------- #
def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Export a header tree to mirrored per-header binding files")
    ap.add_argument("config", help="tree config JSON (headerRoot, includeDirs, defines, outputRoot, ...)")
    ap.add_argument("--report", help="write the coverage report (JSON) here")
    ap.add_argument("--scan-cache", help="path to cache/load the Phase-A scan (ll types, refs, base edges)")
    ap.add_argument("--reuse-scan", action="store_true",
                    help="reuse the --scan-cache instead of re-parsing every header (Phase-B-only rerun)")
    args = ap.parse_args(argv)

    with open(args.config, "r", encoding="utf-8-sig") as handle:
        cfg = json.load(handle)
    print(f"libclang: {ph.configure_libclang(cfg.get('libclangPath'))}")

    header_root = cfg["headerRoot"]
    include_dirs = cfg.get("includeDirs", [])
    server_root = cfg["outputRoot"]
    client_root = cfg.get("clientOutputRoot")
    prefix = cfg.get("namespacePrefix", "ll")
    closure_roots = cfg.get("closureRoots", ["ll", "mc"])
    skip_headers: Set[str] = set(cfg.get("skipHeaders", []))
    base_cfg = {
        "std": cfg.get("std", "c++20"),
        "includeDirs": include_dirs,
        "defines": cfg.get("defines", []),
        "exclude": list(cfg.get("exclude", [])) + ["^std::", "^::std::"],
        "knownNativeClasses": cfg.get("knownNativeClasses", []),
        "allowStringView": cfg.get("allowStringView", False),
        "requireApiMacro": True,
        "apiMacros": cfg.get("apiMacros", ph.DEFAULT_API_MACROS),
        "templateClasses": [], "templateFunctions": [], "rename": {},
    }
    parse_args = ph.build_parse_args(base_cfg)
    index = cindex.Index.create()

    seed_headers = [h for h in enumerate_headers(header_root)
                    if ph.include_relative(h, include_dirs) is not None]
    print(f"enumerated {len(seed_headers)} seed header(s) under {header_root}")

    # ---- Phase A: global scan (memory-bounded), optionally cached ---------- #
    resolver = ph.HeaderResolver(include_dirs)
    tmpl_resolver = TemplateResolver(index, parse_args, resolver, include_dirs, closure_roots)
    cache_path = args.scan_cache
    loaded = None
    if args.reuse_scan and cache_path and os.path.exists(cache_path):
        with open(cache_path, "r", encoding="utf-8") as handle:
            loaded = json.load(handle)
        print(f"[A] reusing scan cache: {cache_path}")
    if loaded is not None:
        ll_types = loaded["llTypes"]
        refs = set(loaded["refs"])
        base_keep = set(loaded["baseKeep"])
        unclean = set(loaded["unclean"])
    else:
        print("[A] scanning seed headers for the global type registry...")
        ll_types, refs, base_keep, unclean = scan_headers(index, seed_headers, include_dirs, parse_args,
                                                          base_cfg, tmpl_resolver)
        if cache_path:
            with open(cache_path, "w", encoding="utf-8", newline="\n") as handle:
                json.dump({"llTypes": ll_types, "refs": sorted(refs), "baseKeep": sorted(base_keep),
                           "unclean": sorted(unclean)}, handle, ensure_ascii=False)
            print(f"[A] scan cache written: {cache_path}")
    skip_headers |= unclean

    external = resolve_external(refs, ll_types, resolver, include_dirs, closure_roots)
    print(f"[A] external (opaque) types to bind: {len(external)}")

    # Global type universe + owners (canonKey -> defining file) per target.
    bound: Set[str] = set()
    script_name_by_cpp: Dict[str, str] = {}
    owners: Dict[str, Dict[str, Dict[str, str]]] = {"server": {}, "client": {}}
    for canon, info in ll_types.items():
        bound.add(canon)
        bound.add(info["cppType"].lstrip(":"))
        script_name_by_cpp[canon] = info["scriptName"]
        script_name_by_cpp[info["cppType"]] = info["scriptName"]
        script_name_by_cpp[info["cppType"].lstrip(":")] = info["scriptName"]
        for target in ("server", "client"):
            fn = derive_names(info["header"], target)[0]
            owners[target].setdefault(canon, {"header_rel": info["header"], "bindFunc": fn})
            owners[target].setdefault(info["cppType"].lstrip(":"), {"header_rel": info["header"], "bindFunc": fn})
    for canon, info in external.items():
        bound.add(canon)
        bound.add(info["cppType"].lstrip(":"))
        script_name_by_cpp[canon] = info["scriptName"]
        script_name_by_cpp[info["cppType"]] = info["scriptName"]
        for target in ("server", "client"):
            fn = derive_names(info["header"], target)[0]
            owners[target].setdefault(canon, {"header_rel": info["header"], "bindFunc": fn})

    ctx = ph.TypeCtx(bound, set(base_cfg.get("knownNativeClasses", [])),
                     base_cfg.get("allowStringView", False), script_name_by_cpp)

    report: Dict[str, Any] = {"llTypes": len(ll_types), "externalTypes": len(external),
                              "skippedHeaders": sorted(skip_headers), "files": {}}
    grand: Dict[str, List[int]] = {}

    # ---- Phase B: per-header emit (memory-bounded) ------------------------ #
    for target, out_root in (("server", server_root), ("client", client_root)):
        if out_root is None:
            continue
        print(f"[B:{target}] emitting per-header binding files...")
        registrar: List[Tuple[str, str]] = []
        deps: Dict[str, Set[str]] = {}
        totals = [0, 0, 0]
        written: Set[str] = set()
        for header in seed_headers:
            header_rel = ph.include_relative(header, include_dirs)
            if header_rel is None or header_rel in skip_headers:
                continue
            tu = ph.parse_one(index, header, parse_args)
            if any(d.severity >= cindex.Diagnostic.Error for d in tu.diagnostics):
                del tu
                continue
            func_name, namespace, out_rel = derive_names(header_rel, target)
            ex_cfg = dict(base_cfg)
            ex_cfg.update({"namespaces": None, "include": None, "apiTarget": target,
                           "exportPolicy": "allExceptSkip", "scriptNamespace": namespace,
                           "functionName": func_name, "emitIncludes": [header_rel],
                           "keepTypes": base_keep})
            extractor = ph.Extractor(ex_cfg, {os.path.normcase(os.path.abspath(header))})
            extractor.run(tu)
            extractor.expand_base_instantiations(tmpl_resolver)
            spec = ph.build_spec(extractor, ctx, ex_cfg)
            del extractor, tu
            if not spec["classes"] and not spec["functions"]:
                continue
            wire_cross_file(spec, owners[target], deps)
            written.add(os.path.normcase(os.path.abspath(write_spec(out_root, out_rel, spec))))
            registrar.append((func_name, out_rel))
            totals[0] += 1
            totals[1] += len(spec["classes"])
            totals[2] += len(spec["functions"])
            report["files"][f"{target}:{out_rel}"] = {
                "classes": len(spec["classes"]), "functions": len(spec["functions"]),
                "extraNativeClasses": len(spec["extraNativeClasses"])}

        # ---- Phase C: external opaque files ------------------------------- #
        ext_by_header: Dict[str, List[Dict[str, Any]]] = {}
        for canon, info in external.items():
            ext_by_header.setdefault(info["header"], []).append(info)
        ext_count = 0
        for ext_header in sorted(ext_by_header):
            if ext_header in skip_headers:
                continue
            func_name, namespace, out_rel = derive_names(ext_header, target)
            classes = [{"scriptName": inf["scriptName"], "cppType": inf["cppType"], "base": None,
                        "baseCppType": None, "constructible": False, "methods": [],
                        "properties": [], "staticMethods": []} for inf in ext_by_header[ext_header]]
            spec = {"namespace": namespace, "functionName": func_name, "includes": [ext_header],
                    "classes": classes, "functions": [], "extraNativeClasses": []}
            written.add(os.path.normcase(os.path.abspath(write_spec(out_root, out_rel, spec))))
            registrar.append((func_name, out_rel))
            totals[0] += 1
            totals[1] += len(classes)
            ext_count += 1
            report["files"][f"{target}:{out_rel}"] = {"classes": len(classes), "external": True}

        registrar = topo_sort_registrar(registrar, deps)
        if registrar:
            entry = f"bindAllGenerated{prefix.capitalize()}" + ("" if target == "server" else "Client")
            path = emit_registrar(out_root, prefix, registrar, entry)
            written.add(os.path.normcase(os.path.abspath(path)))
            print(f"  registrar: {path} ({len(registrar)} bind functions)")
        removed = clean_stale(out_root, written)
        if removed:
            print(f"  removed {removed} stale file(s) not produced this run")
        print(f"[B:{target}] {totals[0]} file(s) ({ext_count} external), {totals[1]} classes, {totals[2]} functions")
        grand[target] = totals

    print("\n=== totals ===")
    for target, (nf, nc, nfn) in grand.items():
        print(f"{target}: {nf} file(s), {nc} classes, {nfn} functions")
    if args.report:
        with open(args.report, "w", encoding="utf-8", newline="\n") as handle:
            json.dump({"totals": grand, "summary": {k: report[k] for k in ("llTypes", "externalTypes", "skippedHeaders")},
                       "files": report["files"]}, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
        print(f"report: {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
