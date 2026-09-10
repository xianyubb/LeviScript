#!/usr/bin/env python3
"""LeviScript header-driven binding generator (libclang front end).

This is the "read the headers, generate the bindings" program. It parses real
LeviLamina / Minecraft C++ headers with libclang and produces a binding spec
(the JSON consumed by autobind.py), covering:

  * namespaces            -> grouped into the target script namespace
  * classes / structs     -> ClassBinder::registerClass<T, Base> (inheritance kept)
  * member functions      -> ClassBinder::method<T> (const / static aware, overloads
                             disambiguated with an explicit static_cast)
  * data members          -> ClassBinder::property<T>(..., &T::field)
  * constructors          -> ClassBinder::constructor<T> (script `new T(...)`)
  * free functions        -> namespace functions via makeNativeFunction
  * class templates       -> bound for the instantiations listed in the config
  * function templates    -> bound for the instantiations listed in the config

Only PUBLIC, non-operator declarations whose signatures use types the runtime
binder understands are emitted; everything else is reported as skipped so nothing
silently fails to compile.

Typical use (two stage pipeline):
    python parse_headers.py config.json --spec out_spec.json
    python autobind.py out_spec.json -o GeneratedApi.cpp --emit-expected expected.json
or one shot:
    python parse_headers.py config.json --cpp GeneratedApi.cpp

libclang is located from, in order: config "libclangPath", the LIBCLANG_PATH env
var, "C:/Program Files/LLVM/bin/libclang.dll", then the pip `libclang` package.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional, Set, Tuple

from clang import cindex
from clang.cindex import AccessSpecifier, CursorKind, TypeKind


# --------------------------------------------------------------------------- #
# libclang setup
# --------------------------------------------------------------------------- #
def configure_libclang(explicit: Optional[str]) -> str:
    candidates: List[str] = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("LIBCLANG_PATH")
    if env:
        candidates.append(env if env.lower().endswith(".dll") else os.path.join(env, "libclang.dll"))
    candidates.append(r"C:\Program Files\LLVM\bin\libclang.dll")
    for candidate in candidates:
        if candidate and os.path.exists(candidate):
            cindex.Config.set_library_file(candidate)
            return candidate
    return "auto (pip libclang / system default)"


INTEGER_KINDS = {
    TypeKind.CHAR_U, TypeKind.CHAR_S, TypeKind.SCHAR, TypeKind.UCHAR,
    TypeKind.SHORT, TypeKind.USHORT, TypeKind.INT, TypeKind.UINT,
    TypeKind.LONG, TypeKind.ULONG, TypeKind.LONGLONG, TypeKind.ULONGLONG,
    TypeKind.CHAR16, TypeKind.CHAR32,
}
FLOAT_KINDS = {TypeKind.FLOAT, TypeKind.DOUBLE, TypeKind.LONGDOUBLE}
POINTER_KINDS = {TypeKind.POINTER, TypeKind.LVALUEREFERENCE, TypeKind.RVALUEREFERENCE}


# --------------------------------------------------------------------------- #
# name helpers
# --------------------------------------------------------------------------- #
_CONTAINER_PARENTS = {
    CursorKind.NAMESPACE, CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL,
    CursorKind.UNION_DECL, CursorKind.CLASS_TEMPLATE,
}

# Declaration kinds we would otherwise bind; only these are screened for skip macros
# (so MACRO_DEFINITION and other cursors do not trigger false positives).
_BINDABLE_DECL_KINDS = {
    CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL, CursorKind.CLASS_TEMPLATE, CursorKind.FUNCTION_DECL,
}


def qualified_name(cursor) -> str:
    parts: List[str] = []
    node = cursor
    while node is not None and node.kind != CursorKind.TRANSLATION_UNIT:
        if node.kind in _CONTAINER_PARENTS and node.spelling:
            parts.append(node.spelling)
        node = node.semantic_parent
    parts.reverse()
    return "::".join(parts)


def cpp_type_of(cursor) -> str:
    return "::" + qualified_name(cursor)


def first_template_arg(canonical: str) -> Optional[str]:
    start = canonical.find("<")
    if start < 0:
        return None
    depth = 0
    arg_start = start + 1
    for i in range(start, len(canonical)):
        ch = canonical[i]
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
            if depth == 0:
                inner = canonical[arg_start:i]
                return split_top_level(inner)[0].strip()
        elif ch == "," and depth == 1:
            return canonical[arg_start:i].strip()
    return None


def split_top_level(text: str) -> List[str]:
    out, depth, current = [], 0, []
    for ch in text:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(current))
            current = []
        else:
            current.append(ch)
    if current:
        out.append("".join(current))
    return out


# --------------------------------------------------------------------------- #
# binding policy: never bind declarations carrying these macros
# --------------------------------------------------------------------------- #
# LLAPI (plain dllexport) is fine to bind. LLNDAPI ([[nodiscard]] API) and MCNAPI
# (Minecraft native-binary symbols) are excluded on request.
SKIP_MACROS = {"LLNDAPI", "MCNAPI"}

_source_cache: Dict[str, List[str]] = {}


def _source_line(cursor) -> str:
    """Return the source line on which `cursor` is located (cached per file)."""
    loc = cursor.location
    if loc is None or loc.file is None:
        return ""
    path = loc.file.name
    lines = _source_cache.get(path)
    if lines is None:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                lines = handle.read().splitlines()
        except OSError:
            lines = []
        _source_cache[path] = lines
    index = loc.line - 1
    return lines[index] if 0 <= index < len(lines) else ""


def has_skip_macro(cursor) -> bool:
    """True if the declaration's source line is prefixed with a skip macro.

    Detection is line/token based rather than AST based: a marker that expands to
    an attribute (LLNDAPI -> [[nodiscard]]) or to nothing (MCNAPI) is not reliably
    part of the cursor's extent, so we inspect the declaration's own source line,
    ignoring trailing // comments. LL/MC place the marker at the start of the
    declaration line.
    """
    code = _source_line(cursor).split("//", 1)[0]
    return any(re.search(r"\b" + macro + r"\b", code) for macro in SKIP_MACROS)


# --------------------------------------------------------------------------- #
# type classification
# --------------------------------------------------------------------------- #
class TypeCtx:
    def __init__(self, bound_classes: Set[str], known_native: Set[str], allow_string_view: bool,
                 script_name_by_cpp: Optional[Dict[str, str]] = None):
        self.bound_classes = bound_classes
        self.known_native = known_native
        self.allow_string_view = allow_string_view
        # maps a fully qualified C++ type ("::a::B" and "a::B") -> script class name
        self.script_name_by_cpp = script_name_by_cpp or {}


def _strip_prefixes(canon: str) -> str:
    for prefix in ("const ", "volatile ", "struct ", "class ", "enum "):
        if canon.startswith(prefix):
            canon = canon[len(prefix):]
    return canon.strip()


def classify(ty, ctx: TypeCtx) -> Tuple[bool, str]:
    """Return (bindable, reason). Conservative: unknown -> not bindable."""
    # Classify on the canonical type so typedefs (uint16_t, size_t, int32_t, ...)
    # resolve to their underlying primitive kind instead of falling through to the
    # record / native-class branch.
    canon_ty = ty.get_canonical()
    kind = canon_ty.kind
    if kind == TypeKind.VOID:
        return True, "void"
    if kind == TypeKind.BOOL:
        return True, "bool"
    if kind in INTEGER_KINDS:
        return True, "integer"
    if kind in FLOAT_KINDS:
        return True, "float"
    if kind == TypeKind.ENUM:
        return True, "enum"

    canon = _strip_prefixes(canon_ty.spelling)
    if canon.startswith("std::basic_string<char") or canon == "std::string":
        return True, "string"
    if canon.startswith("std::basic_string_view<char") or "string_view" in canon:
        return (True, "string_view") if ctx.allow_string_view else (False, "string_view param not supported")

    if kind == TypeKind.POINTER:
        pointee = canon_ty.get_pointee()
        pointee_canon = _strip_prefixes(pointee.get_canonical().spelling)
        if pointee_canon.startswith("std::basic_string<char"):
            return True, "string"
        if pointee.get_canonical().kind in INTEGER_KINDS and pointee_canon == "char":
            # const char* is fine as a return, unsafe as a parameter -> reject params
            return False, "char* parameter not supported (use std::string)"
        decl = pointee.get_declaration()
        name = qualified_name(decl) if (decl is not None and decl.kind in _CONTAINER_PARENTS) else pointee_canon
        name = _strip_prefixes(name)
        if (name in ctx.bound_classes or name in ctx.known_native
                or ("::" + name) in ctx.bound_classes or name == "std::string"):
            return True, "native"
        # Any other raw pointer is exposed through the universal pointer system.
        return True, "pointer"

    if kind in (TypeKind.LVALUEREFERENCE, TypeKind.RVALUEREFERENCE):
        pointee = canon_ty.get_pointee()
        pointee_canon = _strip_prefixes(pointee.get_canonical().spelling)
        if pointee_canon.startswith("std::basic_string<char"):
            return True, "string"
        return classify_pointee_class(pointee, ctx)

    # std containers
    if canon.startswith("std::vector<") or canon.startswith("std::optional<"):
        arg = first_template_arg(canon)
        if arg is None:
            return False, "malformed container"
        return classify_spelling(_strip_prefixes(arg), ctx)
    if canon.startswith("std::shared_ptr<") or canon.startswith("std::unique_ptr<"):
        arg = first_template_arg(canon)
        if arg is None:
            return False, "malformed smart pointer"
        return classify_native_name(_strip_prefixes(arg), ctx)

    if kind == TypeKind.RECORD or kind == TypeKind.UNEXPOSED:
        return classify_native_name(_strip_prefixes(canon), ctx)

    return False, f"unsupported type '{ty.spelling}'"


def classify_pointee_class(pointee, ctx: TypeCtx) -> Tuple[bool, str]:
    decl = pointee.get_declaration()
    name = None
    if decl is not None and decl.kind in _CONTAINER_PARENTS:
        name = qualified_name(decl)
    if name is None:
        name = _strip_prefixes(pointee.get_canonical().spelling)
    return classify_native_name(name, ctx)


def classify_native_name(name: str, ctx: TypeCtx) -> Tuple[bool, str]:
    name = _strip_prefixes(name)
    if name in ctx.bound_classes or name in ctx.known_native or ("::" + name) in ctx.bound_classes:
        return True, "native"
    if name in ("std::string",):
        return True, "string"
    return False, f"native class not bound: '{name}'"


def classify_spelling(spelling: str, ctx: TypeCtx) -> Tuple[bool, str]:
    """Classify a template argument that we only have as a string."""
    s = _strip_prefixes(spelling)
    if s in ("bool",):
        return True, "bool"
    if s in NUMERIC_NAMES:
        return True, "integer"
    if s in ("float", "double", "long double"):
        return True, "float"
    if s.startswith("std::basic_string<char") or s == "std::string":
        return True, "string"
    return classify_native_name(s, ctx)


NUMERIC_NAMES = {
    "char", "signed char", "unsigned char", "short", "unsigned short", "int", "unsigned int",
    "long", "unsigned long", "long long", "unsigned long long", "int8_t", "uint8_t", "int16_t",
    "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t", "size_t", "ptrdiff_t",
    "wchar_t", "char16_t", "char32_t",
}


# --------------------------------------------------------------------------- #
# C++ -> TypeScript type mapping (for .d.ts emission)
# --------------------------------------------------------------------------- #
def _script_name_of_type(ty, ctx: TypeCtx) -> Optional[str]:
    decl = ty.get_declaration()
    if decl is not None and decl.kind in _CONTAINER_PARENTS:
        name = qualified_name(decl)
    else:
        name = _strip_prefixes(ty.get_canonical().spelling)
    name = _strip_prefixes(name)
    return ctx.script_name_by_cpp.get("::" + name) or ctx.script_name_by_cpp.get(name)


def ts_type_spelling(spelling: str, ctx: TypeCtx) -> str:
    s = _strip_prefixes(spelling)
    if s == "bool":
        return "boolean"
    if s in NUMERIC_NAMES or s in ("float", "double", "long double"):
        return "number"
    if s.startswith("std::basic_string<char") or s == "std::string":
        return "string"
    return ctx.script_name_by_cpp.get("::" + s) or ctx.script_name_by_cpp.get(s) or "any"


def ts_type(ty, ctx: TypeCtx) -> str:
    canon_ty = ty.get_canonical()
    kind = canon_ty.kind
    if kind == TypeKind.VOID:
        return "void"
    if kind == TypeKind.BOOL:
        return "boolean"
    if kind in INTEGER_KINDS or kind in FLOAT_KINDS or kind == TypeKind.ENUM:
        return "number"
    canon = _strip_prefixes(canon_ty.spelling)
    if canon.startswith("std::basic_string<char") or canon.startswith("std::basic_string_view<char"):
        return "string"
    if kind == TypeKind.POINTER:
        pointee = canon_ty.get_pointee()
        pc = _strip_prefixes(pointee.get_canonical().spelling)
        if pc.startswith("std::basic_string<char"):
            return "string"
        if pointee.get_canonical().kind in INTEGER_KINDS and pc == "char":
            return "string"
        return _script_name_of_type(pointee, ctx) or "NativePointer"
    if kind in (TypeKind.LVALUEREFERENCE, TypeKind.RVALUEREFERENCE):
        pointee = canon_ty.get_pointee()
        pc = _strip_prefixes(pointee.get_canonical().spelling)
        if pc.startswith("std::basic_string<char"):
            return "string"
        return _script_name_of_type(pointee, ctx) or ts_type(pointee, ctx)
    if canon.startswith("std::vector<"):
        arg = first_template_arg(canon)
        return f"{ts_type_spelling(arg, ctx)}[]" if arg else "any[]"
    if canon.startswith("std::optional<"):
        arg = first_template_arg(canon)
        return f"({ts_type_spelling(arg, ctx)} | null)" if arg else "any"
    if canon.startswith("std::shared_ptr<") or canon.startswith("std::unique_ptr<"):
        arg = first_template_arg(canon)
        nm = _strip_prefixes(arg) if arg else ""
        return ctx.script_name_by_cpp.get("::" + nm) or ctx.script_name_by_cpp.get(nm) or "object"
    if kind == TypeKind.RECORD:
        return _script_name_of_type(ty, ctx) or "any"
    return "any"


def ts_params(cursor, ctx: TypeCtx) -> List[Dict[str, str]]:
    out = []
    for i, arg in enumerate(cursor.get_arguments()):
        out.append({"name": arg.spelling or f"arg{i}", "type": ts_type(arg.type, ctx)})
    return out


def signature_bindable(cursor, ctx: TypeCtx) -> Tuple[bool, str]:
    result = cursor.result_type
    ok, reason = classify(result, ctx)
    if not ok and result.kind != TypeKind.VOID:
        return False, f"return {reason}"
    for arg in cursor.get_arguments():
        ok, reason = classify(arg.type, ctx)
        if not ok:
            return False, f"arg '{arg.spelling or arg.type.spelling}': {reason}"
    return True, "ok"


# --------------------------------------------------------------------------- #
# extraction
# --------------------------------------------------------------------------- #
class Extractor:
    def __init__(self, config: Dict[str, Any], target_files: Set[str]):
        self.config = config
        self.target_files = target_files
        self.classes: Dict[str, Dict[str, Any]] = {}   # qualified name -> class spec
        self.functions: List[Dict[str, Any]] = []
        self.skipped: List[str] = []
        self.template_class_instantiations: Dict[str, List[Dict[str, str]]] = {}
        for entry in config.get("templateClasses", []):
            self.template_class_instantiations.setdefault(entry["templateOf"], []).append(entry)
        self.template_functions: List[Dict[str, str]] = config.get("templateFunctions", [])

    def in_target(self, cursor) -> bool:
        loc = cursor.location
        if loc is None or loc.file is None:
            return False
        return os.path.normcase(os.path.abspath(loc.file.name)) in self.target_files

    def run(self, tu) -> None:
        for cursor in tu.cursor.get_children():
            self.visit(cursor)

    def visit(self, cursor) -> None:
        if cursor.kind == CursorKind.NAMESPACE:
            for child in cursor.get_children():
                self.visit(child)
            return
        if not self.in_target(cursor):
            return
        if cursor.kind in _BINDABLE_DECL_KINDS and has_skip_macro(cursor):
            qname = qualified_name(cursor)
            label = (f"{qname}::{cursor.spelling}" if cursor.kind == CursorKind.FUNCTION_DECL
                     else (qname or cursor.spelling))
            self.skipped.append(f"'{label}': marked LLNDAPI/MCNAPI (not bound)")
            return
        if cursor.kind in (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL):
            if cursor.is_definition():
                self.collect_class(cursor, None)
        elif cursor.kind == CursorKind.CLASS_TEMPLATE:
            self.collect_template_class(cursor)
        elif cursor.kind == CursorKind.FUNCTION_DECL:
            self.collect_function(cursor)

    # -- classes ----------------------------------------------------------- #
    def collect_class(self, cursor, instantiated_from: Optional[str]) -> None:
        qname = qualified_name(cursor)
        if not self.selected(qname, cursor.spelling):
            return
        cpp_type = ("::" + qname) if instantiated_from is None else instantiated_from
        entry: Dict[str, Any] = {
            "scriptName": self.script_name_for(cursor.spelling, cpp_type),
            "cppType": cpp_type,
            "base": None,
            "constructible": False,
            "methods": [],
            "properties": [],
            "staticMethods": [],
            "_qname": qname,
            "_cursor": cursor,
        }
        self.classes[qname if instantiated_from is None else cpp_type] = entry

    def collect_template_class(self, cursor) -> None:
        qname = qualified_name(cursor)
        instantiations = self.template_class_instantiations.get("::" + qname, [])
        if not instantiations:
            self.skipped.append(f"template class '{qname}': no instantiation configured (see templateClasses)")
            return
        for inst in instantiations:
            entry = self._build_class_entry(cursor, inst["cpp"], inst.get("scriptName"))
            if entry:
                self.classes[inst["cpp"]] = entry

    def _build_class_entry(self, cursor, cpp_type: str, script_name: Optional[str]) -> Optional[Dict[str, Any]]:
        qname = qualified_name(cursor)
        return {
            "scriptName": script_name or self.sanitize(cpp_type.split("::")[-1]),
            "cppType": cpp_type,
            "base": None,
            "constructible": False,
            "methods": [],
            "properties": [],
            "staticMethods": [],
            "_qname": qname,
            "_cursor": cursor,
            # members of an instantiation keep dependent types in the AST, so the
            # signature classifier cannot see through them; trust the config.
            "_templateInstantiation": True,
        }

    def selected(self, qname: str, leaf: str) -> bool:
        namespaces = self.config.get("namespaces")
        if namespaces:
            if not any(qname == ns or qname.startswith(ns + "::") for ns in namespaces):
                return False
        include = self.config.get("include")
        if include and not any(re.search(p, leaf) or re.search(p, qname) for p in include):
            return False
        exclude = self.config.get("exclude")
        if exclude and any(re.search(p, leaf) or re.search(p, qname) for p in exclude):
            return False
        return True

    def sanitize(self, text: str) -> str:
        return re.sub(r"[^A-Za-z0-9_]", "_", text)

    def script_name_for(self, leaf: str, cpp_type: str) -> str:
        mapping = self.config.get("rename", {})
        return mapping.get(cpp_type, mapping.get(leaf, leaf))

    # -- free functions ---------------------------------------------------- #
    def collect_function(self, cursor) -> None:
        parent_qname = qualified_name(cursor.semantic_parent)
        full = (parent_qname + "::" + cursor.spelling) if parent_qname else cursor.spelling
        if not self.selected(parent_qname, cursor.spelling):
            return
        if cursor.spelling.startswith("operator"):
            self.skipped.append(f"operator function '{full}'")
            return
        self.functions.append({"_qname": full, "_cursor": cursor, "_cpp": "&::" + full})


# --------------------------------------------------------------------------- #
# second pass: fill members once the bound-class set is known
# --------------------------------------------------------------------------- #
def member_pointer_type(cursor, cpp_type: str) -> str:
    result = cursor.result_type.spelling
    args = ", ".join(a.type.spelling for a in cursor.get_arguments())
    const = " const" if cursor.is_const_method() else ""
    return f"{result} ({cpp_type}::*)({args}){const}"


def fill_members(extractor: Extractor, ctx: TypeCtx) -> None:
    for entry in list(extractor.classes.values()):
        cursor = entry["_cursor"]
        cpp_type = entry["cppType"]
        trust = entry.get("_templateInstantiation", False)
        method_counts: Dict[str, int] = {}
        for child in cursor.get_children():
            if child.kind == CursorKind.CXX_METHOD:
                method_counts[child.spelling] = method_counts.get(child.spelling, 0) + 1

        for child in cursor.get_children():
            if child.access_specifier != AccessSpecifier.PUBLIC:
                continue
            if child.kind == CursorKind.CXX_METHOD:
                if has_skip_macro(child):
                    extractor.skipped.append(f"{cpp_type}::{child.spelling}: marked LLNDAPI/MCNAPI (not bound)")
                else:
                    emit_method(extractor, entry, child, cpp_type, ctx, method_counts, trust)
            elif child.kind == CursorKind.FIELD_DECL:
                ts = ts_type(child.type, ctx)
                if trust:
                    entry["properties"].append({"scriptName": child.spelling, "field": f"&{cpp_type}::{child.spelling}", "tsType": ts})
                    continue
                ok, reason = classify(child.type, ctx)
                if ok:
                    entry["properties"].append({"scriptName": child.spelling, "field": f"&{cpp_type}::{child.spelling}", "tsType": ts})
                else:
                    extractor.skipped.append(f"{cpp_type}::{child.spelling} (field): {reason}")
            elif child.kind == CursorKind.CONSTRUCTOR:
                emit_constructor(extractor, entry, child, cpp_type, ctx, trust)

    # resolve bases now that all bound classes are known
    for entry in extractor.classes.values():
        cursor = entry["_cursor"]
        for child in cursor.get_children():
            if child.kind == CursorKind.CXX_BASE_SPECIFIER and child.access_specifier == AccessSpecifier.PUBLIC:
                base_decl = child.referenced
                base_qname = qualified_name(base_decl) if base_decl is not None else ""
                if base_qname in extractor.classes:
                    entry["base"] = extractor.classes[base_qname]["scriptName"]
                elif base_qname:
                    extractor.skipped.append(f"{entry['cppType']}: base '{base_qname}' not bound, inheritance dropped")
                break


def emit_method(extractor, entry, cursor, cpp_type, ctx, method_counts, trust) -> None:
    name = cursor.spelling
    if name.startswith("operator"):
        extractor.skipped.append(f"{cpp_type}::{name}: operator method")
        return
    overloaded = method_counts.get(name, 0) > 1
    if trust:
        if overloaded:
            extractor.skipped.append(f"{cpp_type}::{name}: overloaded template method (bind manually)")
            return
        expr = f"&{cpp_type}::{name}"
    else:
        ok, reason = signature_bindable(cursor, ctx)
        if not ok:
            extractor.skipped.append(f"{cpp_type}::{name}: {reason}")
            return
        expr = (f"static_cast<{member_pointer_type(cursor, cpp_type)}>(&{cpp_type}::{name})"
                if overloaded else f"&{cpp_type}::{name}")
    bucket = "staticMethods" if cursor.is_static_method() else "methods"
    entry[bucket].append({
        "scriptName": name,
        "cpp": expr,
        "tsParams": ts_params(cursor, ctx),
        "tsReturns": ts_type(cursor.result_type, ctx),
    })


def emit_constructor(extractor, entry, cursor, cpp_type, ctx, trust) -> None:
    if entry["constructible"]:
        return
    args = list(cursor.get_arguments())
    if trust:
        # Dependent parameter types cannot be reconstructed; only a default
        # constructor is safe to synthesize for a template instantiation.
        if args:
            return
        entry["constructor"] = f"+[]() -> {cpp_type}* {{ return new {cpp_type}(); }}"
        entry["constructible"] = True
        entry["tsConstructorParams"] = []
        return
    if not all(classify(a.type, ctx)[0] for a in args):
        return
    arg_types = ", ".join(a.type.spelling for a in args)
    forwards = ", ".join(f"std::move(a{i})" for i in range(len(args)))
    factory = (f"+[]({arg_types}) -> {cpp_type}* {{ return new {cpp_type}({forwards}); }}") \
        if args else f"+[]() -> {cpp_type}* {{ return new {cpp_type}(); }}"
    entry["constructor"] = factory
    entry["constructible"] = True
    entry["tsConstructorParams"] = ts_params(cursor, ctx)


def build_spec(extractor: Extractor, ctx: TypeCtx, config: Dict[str, Any]) -> Dict[str, Any]:
    fill_members(extractor, ctx)

    classes: List[Dict[str, Any]] = []
    for entry in extractor.classes.values():
        classes.append({
            "scriptName": entry["scriptName"],
            "cppType": entry["cppType"],
            "base": entry["base"],
            "constructible": entry["constructible"],
            **({"constructor": entry["constructor"]} if entry.get("constructor") else {}),
            **({"tsConstructorParams": entry["tsConstructorParams"]} if entry.get("tsConstructorParams") is not None else {}),
            "methods": entry["methods"],
            "properties": entry["properties"],
            "staticMethods": entry["staticMethods"],
        })

    functions: List[Dict[str, Any]] = []
    for fn in extractor.functions:
        cursor = fn["_cursor"]
        ok, reason = signature_bindable(cursor, ctx)
        if not ok:
            extractor.skipped.append(f"function '{fn['_qname']}': {reason}")
            continue
        functions.append({"scriptName": cursor.spelling, "cpp": fn["_cpp"],
                          "tsParams": ts_params(cursor, ctx), "tsReturns": ts_type(cursor.result_type, ctx)})
    for tf in extractor.template_functions:
        functions.append({"scriptName": tf["scriptName"], "cpp": tf["cpp"]})

    return {
        "namespace": config.get("scriptNamespace", "native"),
        "functionName": config.get("functionName"),
        "includes": config.get("emitIncludes", []),
        "classes": classes,
        "functions": functions,
    }


# --------------------------------------------------------------------------- #
# driver
# --------------------------------------------------------------------------- #
def parse(config: Dict[str, Any]) -> Any:
    args = ["-x", "c++", "-std=" + config.get("std", "c++20"),
            "-fms-extensions", "-fms-compatibility", "--target=x86_64-pc-windows-msvc",
            "-D_WIN32", "-DWIN32", "-D_CRT_SECURE_NO_WARNINGS"]
    for define in config.get("defines", []):
        args.append("-D" + define)
    for inc in config.get("includeDirs", []):
        args.append("-I" + inc)

    index = cindex.Index.create()
    translation_units = []
    for header in config["headers"]:
        tu = index.parse(header, args=args, options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
        diags = [d for d in tu.diagnostics if d.severity >= cindex.Diagnostic.Error]
        if diags:
            print(f"warning: {header}: {len(diags)} error diagnostic(s) while parsing "
                  f"(first: {diags[0].spelling})", file=sys.stderr)
        translation_units.append((header, tu))
    return translation_units


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Generate a LeviScript binding spec from C++ headers via libclang")
    ap.add_argument("config", help="JSON config: headers, includeDirs, defines, namespaces, filters, templates")
    ap.add_argument("--spec", help="write the generated binding spec JSON here")
    ap.add_argument("--cpp", help="also run autobind.py and write generated C++ here")
    ap.add_argument("--emit-expected", help="with --cpp, also write the expected API surface JSON")
    ap.add_argument("--dts", help="write a TypeScript declaration (.d.ts) for the bound namespace")
    ap.add_argument("--report", help="write the skip report (JSON) here")
    args = ap.parse_args(argv)

    try:
        with open(args.config, "r", encoding="utf-8-sig") as handle:
            config = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error reading config: {exc}", file=sys.stderr)
        return 2

    used = configure_libclang(config.get("libclangPath"))
    print(f"libclang: {used}")

    target_files = {os.path.normcase(os.path.abspath(h)) for h in config["headers"]}
    translation_units = parse(config)

    extractor = Extractor(config, target_files)
    for _header, tu in translation_units:
        extractor.run(tu)

    bound = {e["cppType"].lstrip(":") for e in extractor.classes.values()}
    bound |= {qualified for qualified in extractor.classes.keys()}
    known_native = set(config.get("knownNativeClasses", []))
    script_name_by_cpp: Dict[str, str] = {}
    for e in extractor.classes.values():
        script_name_by_cpp[e["cppType"]] = e["scriptName"]
        script_name_by_cpp[e["cppType"].lstrip(":")] = e["scriptName"]
    ctx = TypeCtx(bound, known_native, config.get("allowStringView", False), script_name_by_cpp)

    spec = build_spec(extractor, ctx, config)
    if not spec.get("functionName"):
        spec["functionName"] = f"bindGenerated{spec['namespace'].capitalize()}Api"

    if not args.spec and not args.cpp:
        args.spec = "binding_spec.json"

    if args.spec:
        with open(args.spec, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(spec, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
        print(f"spec: {args.spec} ({len(spec['classes'])} classes, {len(spec['functions'])} functions)")

    if args.report:
        with open(args.report, "w", encoding="utf-8", newline="\n") as handle:
            json.dump({"skipped": extractor.skipped}, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
        print(f"report: {args.report} ({len(extractor.skipped)} skipped)")

    if args.cpp:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import autobind
        source = autobind.generate_cpp(spec)
        with open(args.cpp, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(source)
        print(f"generated C++: {args.cpp}")
        if args.emit_expected:
            expected = autobind.build_expected(spec)
            with open(args.emit_expected, "w", encoding="utf-8", newline="\n") as handle:
                json.dump(expected, handle, indent=2, ensure_ascii=False)
                handle.write("\n")
            print(f"expected API: {args.emit_expected}")

    if args.dts:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import autobind
        with open(args.dts, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(autobind.build_dts(spec))
        print(f"generated d.ts: {args.dts}")

    if not spec["classes"] and not spec["functions"]:
        print("warning: nothing was bound - check namespaces/include filters", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
