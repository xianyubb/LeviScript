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


def include_relative(path: Optional[str], include_dirs: List[str]) -> Optional[str]:
    """Turn an absolute header path into an #include path relative to an include dir
    (e.g. .../include/mc/world/level/BlockPos.h -> mc/world/level/BlockPos.h).
    Returns None if the path is not under any configured include dir."""
    if not path:
        return None
    abs_path = os.path.abspath(path)
    norm = os.path.normcase(abs_path)
    for directory in include_dirs:
        base = os.path.abspath(directory)
        basen = os.path.normcase(base)
        if norm == basen or norm.startswith(basen + os.sep):
            return os.path.relpath(abs_path, base).replace("\\", "/")
    return None


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


# Allow-list policy: export ONLY declarations carrying one of these API markers.
# Server-side and client-side markers route to different output trees.
DEFAULT_API_MACROS = {"server": ["LLAPI", "LLNDAPI", "MCAPI"], "client": ["LLCAPI"]}


def _line_has_macro(cursor, macro: str) -> bool:
    code = _source_line(cursor).split("//", 1)[0]
    return re.search(r"\b" + macro + r"\b", code) is not None


def api_target(cursor, class_cursor, macros: Dict[str, List[str]]) -> Optional[str]:
    """'client' / 'server' if the declaration (or, for members, its enclosing class)
    carries one of the API markers for that target; None if it is not exported API.
    A member's own marker wins over the class-level marker."""
    for target in ("client", "server"):
        for macro in macros.get(target, []):
            if _line_has_macro(cursor, macro):
                return target
    if class_cursor is not None:
        for target in ("client", "server"):
            for macro in macros.get(target, []):
                if _line_has_macro(class_cursor, macro):
                    return target
    return None


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


def classify(ty, ctx: TypeCtx, as_param: bool = False) -> Tuple[bool, str]:
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
    if canon.startswith("std::filesystem::path"):
        return True, "path"
    if canon.startswith("std::basic_string_view<char") or canon == "std::string_view":
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
        non_const_lvalue = (kind == TypeKind.LVALUEREFERENCE and not pointee.is_const_qualified())
        if pointee_canon.startswith("std::basic_string<char") or pointee_canon.startswith("std::filesystem::path"):
            if non_const_lvalue:
                return False, "non-const string/path& output parameter not supported"
            return True, "string"
        # native classes bind by reference (const& and &): unwrap the live object
        native_ok, native_reason = classify_pointee_class(pointee, ctx)
        if native_ok and native_reason == "native":
            return True, "native"
        # scalars / enums / containers by const& (or &&) marshal by value; a non-const
        # lvalue reference is an output parameter that cannot round-trip -> reject
        pk = pointee.get_canonical().kind
        is_value = (pk in INTEGER_KINDS or pk in FLOAT_KINDS or pk == TypeKind.BOOL or pk == TypeKind.ENUM
                    or pointee_canon.startswith("std::optional<") or pointee_canon.startswith("std::vector<"))
        if is_value:
            if non_const_lvalue:
                return False, "non-const value& output parameter not supported"
            return classify(pointee, ctx)
        return False, f"unsupported reference '{canon_ty.spelling}'"

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
        ok, reason = classify_native_name(_strip_prefixes(canon), ctx)
        if ok and reason == "native" and as_param:
            # A native class passed BY VALUE as a parameter would need a copy, which
            # move-only types (e.g. ll::Error) delete; by-value returns are fine (move).
            return False, "native class by value as a parameter is not supported"
        return ok, reason

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
    if s.startswith("std::filesystem::path"):
        return True, "path"
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
    if canon.startswith("std::filesystem::path"):
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
        ok, reason = classify(arg.type, ctx, True)
        if not ok:
            return False, f"arg '{arg.spelling or arg.type.spelling}': {reason}"
    return True, "ok"


# --------------------------------------------------------------------------- #
# closure support: discover native classes referenced by a declaration
# --------------------------------------------------------------------------- #
class Ref:
    """A native (non-std) record type referenced from a bound declaration."""
    __slots__ = ("qname", "cpp_type", "is_specialization", "decl")

    def __init__(self, qname: str, cpp_type: str, is_specialization: bool, decl):
        self.qname = qname                        # template base name for specializations
        self.cpp_type = cpp_type                  # fully qualified, e.g. ::ll::math::intN3<::BlockPos>
        self.is_specialization = is_specialization
        self.decl = decl


def _num_template_args(ty) -> int:
    try:
        n = ty.get_num_template_arguments()
    except Exception:
        return 0
    return n if isinstance(n, int) and n > 0 else 0


def _is_dependent_spec(canon, spelling: str) -> bool:
    """True if a template specialization still has unresolved (dependent) arguments,
    e.g. `intN3<BaseType>`, `FloatN<type-parameter-0-0, ...>` or `boolN<sizeof...(C)>`.
    Such pattern types are not concrete C++ types and cannot be bound."""
    if canon.kind == TypeKind.DEPENDENT:
        return True
    if "type-parameter" in spelling or "type_parameter" in spelling or "sizeof..." in spelling:
        return True
    for i in range(_num_template_args(canon)):
        arg = canon.get_template_argument_type(i)
        if arg is not None and (arg.kind == TypeKind.DEPENDENT or "type-parameter" in arg.spelling):
            return True
    return False


def referenced_classes(cursor) -> List[Ref]:
    """Native record types referenced in a class's member signatures.

    Looks through pointers/references and recurses into template arguments, so
    `std::optional<PreRelease>` yields PreRelease and `intN3<BlockPos>` yields both
    the specialization itself and BlockPos. std:: types are ignored (they are not
    part of the LL/MC API surface).
    """
    out: List[Ref] = []
    seen: Set[str] = set()

    def visit(ty) -> None:
        if ty is None:
            return
        canon = ty.get_canonical()
        if canon.kind in POINTER_KINDS:
            visit(canon.get_pointee())
            return
        count = _num_template_args(canon)
        for i in range(count):
            visit(canon.get_template_argument_type(i))
        decl = canon.get_declaration()
        if decl is None or decl.kind not in _CONTAINER_PARENTS:
            return
        qname = qualified_name(decl)
        if not qname or qname.startswith("std::"):
            return
        spelling = _strip_prefixes(canon.spelling)
        is_spec = count > 0 or "<" in spelling
        if is_spec and _is_dependent_spec(canon, spelling):
            return
        cpp_type = spelling if spelling.startswith("::") else "::" + spelling
        key = cpp_type if is_spec else qname
        if key in seen:
            return
        seen.add(key)
        out.append(Ref(qname, cpp_type, is_spec, decl))

    for child in cursor.get_children():
        k = child.kind
        if k in (CursorKind.CXX_METHOD, CursorKind.CONSTRUCTOR, CursorKind.DESTRUCTOR,
                 CursorKind.FUNCTION_TEMPLATE):
            visit(child.result_type)
            for arg in child.get_arguments():
                visit(arg.type)
        elif k == CursorKind.FIELD_DECL:
            visit(child.type)
        elif k == CursorKind.CXX_BASE_SPECIFIER:
            visit(child.type)
    return out


def referenced_by_function(cursor) -> List[Ref]:
    """Referenced native classes in a free function / method signature (result + args)."""
    out: List[Ref] = []
    seen: Set[str] = set()

    def visit(ty) -> None:
        if ty is None:
            return
        canon = ty.get_canonical()
        if canon.kind in POINTER_KINDS:
            visit(canon.get_pointee())
            return
        count = _num_template_args(canon)
        for i in range(count):
            visit(canon.get_template_argument_type(i))
        decl = canon.get_declaration()
        if decl is None or decl.kind not in _CONTAINER_PARENTS:
            return
        qname = qualified_name(decl)
        if not qname or qname.startswith("std::"):
            return
        spelling = _strip_prefixes(canon.spelling)
        is_spec = count > 0 or "<" in spelling
        cpp_type = spelling if spelling.startswith("::") else "::" + spelling
        key = cpp_type if is_spec else qname
        if key in seen:
            return
        seen.add(key)
        out.append(Ref(qname, cpp_type, is_spec, decl))

    visit(cursor.result_type)
    for arg in cursor.get_arguments():
        visit(arg.type)
    return out


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
        # Global index of every class/struct/class-template DEFINITION seen in any
        # parsed translation unit: qname -> cursor, and leaf name -> [qnames].
        # Used by the closure pass to resolve unfamiliar referenced classes.
        self.definitions: Dict[str, Any] = {}
        self.definitions_by_leaf: Dict[str, List[str]] = {}
        # Export policy:
        #   "allowList"     - only declarations carrying an apiMacros marker for the
        #                     target are exported (data members skipped).
        #   "allExceptSkip" - export EVERY function/method/constructor except those
        #                     carrying a skipMacros marker (e.g. MCNAPI). Client-marked
        #                     declarations (clientMacros, e.g. LLCAPI) route to the
        #                     client target; everything else routes to server.
        # Both keep requireApiMacro True so members are gated through matches_target
        # and data members are not exported (functions/methods only).
        self.require_api_macro = bool(config.get("requireApiMacro", False))
        self.api_target_name = config.get("apiTarget", "server")
        self.api_macros = config.get("apiMacros", DEFAULT_API_MACROS)
        self.export_policy = config.get("exportPolicy", "allowList")
        self.skip_macros = config.get("skipMacros", ["MCNAPI"])
        self.client_macros = config.get("clientMacros", ["LLCAPI"])
        # Restrict closure to types defined under these top-level include dirs (e.g.
        # ["ll", "mc"]) so third-party implementation types (entt/fmt/gsl/...) that
        # appear in signatures are not pulled into the binding tree.
        self.closure_roots = set(config.get("closureRoots", []) or [])
        # Subset of closureRoots whose discovered types are recursed into (their own
        # member-referenced types are bound too). None => recurse every closure root.
        # Restricting this to e.g. ["ll"] binds mc types shallowly (as usable native
        # types) without pulling in the whole Minecraft API graph.
        recurse = config.get("closureRecurseRoots")
        self.closure_recurse_roots = set(recurse) if recurse else None

    def in_target(self, cursor) -> bool:
        loc = cursor.location
        if loc is None or loc.file is None:
            return False
        return os.path.normcase(os.path.abspath(loc.file.name)) in self.target_files

    def _route_all_except_skip(self, cursor, class_cursor=None) -> Optional[str]:
        """'client'/'server' under the allExceptSkip policy, or None if the declaration
        carries a skip marker (e.g. MCNAPI) and must not be exported at all. A
        declaration's own marker wins over its enclosing class's marker."""
        for macro in self.skip_macros:
            if _line_has_macro(cursor, macro):
                return None
        for macro in self.client_macros:
            if _line_has_macro(cursor, macro):
                return "client"
        if class_cursor is not None:
            for macro in self.skip_macros:
                if _line_has_macro(class_cursor, macro):
                    return None
            for macro in self.client_macros:
                if _line_has_macro(class_cursor, macro):
                    return "client"
        return "server"

    def matches_target(self, cursor, class_cursor=None) -> bool:
        """True iff the declaration should be exported to the target being generated.
        Always True when the marker policy is off."""
        if not self.require_api_macro:
            return True
        if self.export_policy == "allExceptSkip":
            return self._route_all_except_skip(cursor, class_cursor) == self.api_target_name
        return api_target(cursor, class_cursor, self.api_macros) == self.api_target_name

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
        if self.require_api_macro:
            # Allow-list: a free function must carry an API marker for this target.
            # Classes are still collected (to host marked members) and pruned later
            # if they end up with nothing exported.
            if cursor.kind == CursorKind.FUNCTION_DECL and not self.matches_target(cursor):
                return
        elif cursor.kind in _BINDABLE_DECL_KINDS and has_skip_macro(cursor):
            qname = qualified_name(cursor)
            label = (f"{qname}::{cursor.spelling}" if cursor.kind == CursorKind.FUNCTION_DECL
                     else (qname or cursor.spelling))
            self.skipped.append(f"'{label}': marked LLNDAPI/MCNAPI (not bound)")
            return
        if cursor.kind in (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL):
            if cursor.is_definition():
                self.collect_class(cursor, None)
                for child in cursor.get_children():
                    if child.kind in (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL,
                                      CursorKind.CLASS_TEMPLATE) \
                            and child.access_specifier == AccessSpecifier.PUBLIC:
                        self.visit(child)  # collect PUBLIC nested classes (e.g. KeyValueDB::WriteBatch);
                        # private nested types (e.g. Logger::PrivateTag) cannot be named outside
        elif cursor.kind == CursorKind.CLASS_TEMPLATE:
            self.collect_template_class(cursor)
        elif cursor.kind == CursorKind.FUNCTION_DECL:
            self.collect_function(cursor)

    # -- classes ----------------------------------------------------------- #
    def index_definitions(self, tu) -> None:
        """Record every class/struct/class-template definition in the whole TU
        (including transitively included headers) so the closure can resolve types
        that are only forward-declared in the target file."""
        scope_kinds = (CursorKind.TRANSLATION_UNIT, CursorKind.NAMESPACE, CursorKind.CLASS_DECL,
                       CursorKind.STRUCT_DECL, CursorKind.CLASS_TEMPLATE, CursorKind.UNION_DECL)

        def walk(cursor) -> None:
            k = cursor.kind
            if k in (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL, CursorKind.CLASS_TEMPLATE) \
                    and cursor.is_definition():
                qname = qualified_name(cursor)
                if qname and qname not in self.definitions:
                    self.definitions[qname] = cursor
                    self.definitions_by_leaf.setdefault(cursor.spelling, []).append(qname)
            if k in scope_kinds:
                for child in cursor.get_children():
                    walk(child)

        walk(tu.cursor)

    def _canon_key(self, cursor, cpp_type: str) -> str:
        """Canonical spelling (leading `::` stripped) used as the cross-file identity of
        a class. Inline namespaces (`namespace ll::event::inline player`) are folded
        away by the canonical spelling, so a base referenced as
        `ll::event::PlayerClickEvent` matches the class whose qualified name is
        `ll::event::player::PlayerClickEvent`."""
        try:
            spelling = _strip_prefixes(cursor.type.get_canonical().spelling).lstrip(":")
            if spelling:
                return spelling
        except Exception:
            pass
        return cpp_type.lstrip(":")

    def collect_class(self, cursor, instantiated_from: Optional[str], force: bool = False):
        qname = qualified_name(cursor)
        if not force and not self.selected(qname, cursor.spelling):
            return None
        cpp_type = ("::" + qname) if instantiated_from is None else instantiated_from
        key = qname if instantiated_from is None else cpp_type
        if key in self.classes:
            return self.classes[key]
        entry: Dict[str, Any] = {
            "scriptName": self.script_name_for(cursor.spelling, cpp_type),
            "cppType": cpp_type,
            "base": None,
            "constructible": False,
            "methods": [],
            "properties": [],
            "staticMethods": [],
            "_qname": qname,
            "_canonKey": self._canon_key(cursor, cpp_type),
            "_refs": set(),
            "_cursor": cursor,
            "_header": include_relative(
                cursor.location.file.name if (cursor.location and cursor.location.file) else None,
                self.config.get("includeDirs", [])),
        }
        self.classes[key] = entry
        return entry

    def _instantiation_script_name(self, template_cursor, cpp_type: str) -> str:
        """Readable, collision-free script name for a concrete specialization:
        template leaf + each argument leaf, e.g. `intN3<::BlockPos>` -> `intN3BlockPos`,
        `Cancellable<::ll::event::PlayerLeftClickEvent>` -> `CancellablePlayerLeftClickEvent`.
        Deriving the name from the whole specialization (not just the last `::`
        component) keeps it distinct from the argument's own class name."""
        base = template_cursor.spelling or "Inst"
        inner = ""
        start, end = cpp_type.find("<"), cpp_type.rfind(">")
        if 0 <= start < end:
            inner = cpp_type[start + 1:end]
        arg_leaves: List[str] = []
        for part in split_top_level(inner):
            leaf = _strip_prefixes(part).lstrip(":").split("::")[-1]
            leaf = re.sub(r"[^A-Za-z0-9]", "", leaf)
            if leaf:
                arg_leaves.append(leaf)
        return self.sanitize(base + "".join(arg_leaves)) or self.sanitize(base)

    def collect_instantiation(self, template_cursor, cpp_type: str):
        """Bind a concrete template specialization discovered by the closure."""
        key = cpp_type.lstrip(":")
        for existing_key in self.classes:
            if existing_key.lstrip(":") == key:
                return self.classes[existing_key]
        clean = self._instantiation_script_name(template_cursor, cpp_type)
        entry = self._build_class_entry(template_cursor, cpp_type, clean)
        if entry:
            self.classes[cpp_type] = entry
        return entry

    def expand_base_instantiations(self) -> None:
        """Bind concrete template-specialization base classes locally.

        `class Derived final : public Template<Args>` loses its inheritance under a
        per-header export because `Template<Args>` is an implicit instantiation that
        no header binds on its own. Discover such bases, bind the concrete
        instantiation in the same file, and resolve the instantiation's own base by
        substituting the template arguments (e.g. `Cancellable<T> : T`).
        """
        for entry in list(self.classes.values()):
            cursor = entry.get("_cursor")
            if cursor is None or entry.get("_templateInstantiation"):
                continue
            for child in cursor.get_children():
                if child.kind != CursorKind.CXX_BASE_SPECIFIER \
                        or child.access_specifier != AccessSpecifier.PUBLIC:
                    continue
                canon = child.type.get_canonical()
                spelling = _strip_prefixes(canon.spelling)
                # A plain (non-template) base is handled by resolve_bases, which may
                # wire it cross-file; only specializations are bound locally here.
                if _num_template_args(canon) <= 0 and "<" not in spelling:
                    break
                if _is_dependent_spec(canon, spelling):
                    break
                template_cursor = canon.get_declaration()
                if template_cursor is None:
                    break
                # get_declaration() on an implicit specialization returns a childless
                # CLASS_DECL; prefer the CLASS_TEMPLATE pattern (indexed from the
                # included template header) which carries the template parameters, the
                # dependent base, and the member declarations.
                pattern = self.definitions.get(qualified_name(template_cursor))
                if pattern is not None and pattern.kind == CursorKind.CLASS_TEMPLATE:
                    template_cursor = pattern
                cpp_type = spelling if spelling.startswith("::") else "::" + spelling
                inst = self.collect_instantiation(template_cursor, cpp_type)
                if inst is not None:
                    self._resolve_instantiation_base(inst, template_cursor, canon)
                break  # only the primary (first) public base is supported

    def _resolve_instantiation_base(self, inst, template_cursor, concrete) -> None:
        """Substitute the template arguments into the specialization's own base so the
        inheritance chain continues past the instantiation (`Cancellable<T> : T`)."""
        if inst.get("_baseResolved"):
            return
        inst["_baseResolved"] = True
        params = [c.spelling for c in template_cursor.get_children()
                  if c.kind == CursorKind.TEMPLATE_TYPE_PARAMETER]
        subst: Dict[str, Any] = {}
        for i in range(min(len(params), _num_template_args(concrete))):
            arg = concrete.get_template_argument_type(i)
            if arg is not None:
                subst[params[i]] = arg
        sub_spelling = None
        for child in template_cursor.get_children():
            if child.kind == CursorKind.CXX_BASE_SPECIFIER and child.access_specifier == AccessSpecifier.PUBLIC:
                raw = _strip_prefixes(child.type.spelling)
                if raw in subst:
                    sub_spelling = _strip_prefixes(subst[raw].get_canonical().spelling)
                else:
                    ct = child.type.get_canonical()
                    cs = _strip_prefixes(ct.spelling)
                    if cs and not _is_dependent_spec(ct, cs):
                        sub_spelling = cs
                break
        if not sub_spelling:
            return
        sub_key = sub_spelling.lstrip(":")
        inst["_baseCppType"] = "::" + sub_key
        inst.setdefault("_refs", set()).add(sub_key)
        normalized = {k.lstrip(":"): v for k, v in self.classes.items()}
        target = normalized.get(sub_key)
        if target is not None and target is not inst:
            inst["base"] = target["scriptName"]
        else:
            inst["_baseUnresolved"] = True

    def expand_closure(self, resolve_header) -> None:
        """Bind every unfamiliar native class reachable from the seed classes.

        `resolve_header(qname, leaf)` locates + parses the definition of a class not
        yet indexed and returns its cursor (or None). There are intentionally no
        caps: termination comes from the `visited` set (each class/instantiation is
        bound once). std:: types are never pulled in. Template specializations are
        bound as concrete instantiations.
        """
        worklist = [e["_cursor"] for e in self.classes.values() if e.get("_cursor") is not None]
        visited: Set[str] = set()
        for entry in self.classes.values():
            if entry.get("_qname"):
                visited.add(entry["_qname"])
            visited.add(entry["cppType"].lstrip(":"))

        while worklist:
            cursor = worklist.pop()
            for ref in referenced_classes(cursor):
                if ref.qname.startswith("std::"):
                    continue
                key = ref.cpp_type.lstrip(":") if ref.is_specialization else ref.qname
                if key in visited:
                    continue
                visited.add(key)

                def_cursor = self.definitions.get(ref.qname)
                if def_cursor is None:
                    def_cursor = resolve_header(ref.qname, ref.qname.split("::")[-1])
                if def_cursor is None:
                    self.skipped.append(f"closure: '{ref.qname}' referenced but its definition was not found")
                    continue

                if self.closure_roots:
                    hdr = include_relative(
                        def_cursor.location.file.name if (def_cursor.location and def_cursor.location.file) else None,
                        self.config.get("includeDirs", []))
                    if hdr is None or hdr.split("/")[0] not in self.closure_roots:
                        self.skipped.append(
                            f"closure: '{ref.qname}' is outside closure roots {sorted(self.closure_roots)} (not bound)")
                        continue

                if ref.is_specialization:
                    # Bind the concrete instantiation but do NOT recurse into it:
                    # walking the generic template's members yields dependent
                    # (non-concrete) pattern types. Its members are emitted in trust
                    # mode from the instantiation entry.
                    self.collect_instantiation(def_cursor, ref.cpp_type)
                else:
                    entry = self.collect_class(def_cursor, None, force=True)
                    if entry is not None and entry.get("_cursor") is not None \
                            and self._should_recurse(entry):
                        worklist.append(entry["_cursor"])

    def _should_recurse(self, entry) -> bool:
        """Whether closure should walk this discovered type's own member references."""
        if self.closure_recurse_roots is None:
            return True
        header = entry.get("_header")
        return bool(header) and header.split("/")[0] in self.closure_recurse_roots

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
            "_canonKey": cpp_type.lstrip(":"),
            "_refs": set(),
            "_cursor": cursor,
            "_header": include_relative(
                cursor.location.file.name if (cursor.location and cursor.location.file) else None,
                self.config.get("includeDirs", [])),
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
        self.functions.append({"_qname": full, "_cursor": cursor, "_cpp": "&::" + full,
                               "_header": include_relative(
                                   cursor.location.file.name if (cursor.location and cursor.location.file) else None,
                                   self.config.get("includeDirs", []))})


# --------------------------------------------------------------------------- #
# second pass: fill members once the bound-class set is known
# --------------------------------------------------------------------------- #
def member_pointer_type(cursor, cpp_type: str) -> str:
    result = cursor.result_type.get_canonical().spelling
    args = ", ".join(a.type.get_canonical().spelling for a in cursor.get_arguments())
    noexcept = " noexcept" if has_noexcept(cursor) else ""
    if cursor.is_static_method():
        # a static member is addressed as a plain function pointer, not a
        # pointer-to-member: `Ret (*)(Args...)`.
        return f"{result} (*)({args}){noexcept}"
    const = " const" if cursor.is_const_method() else ""
    return f"{result} ({cpp_type}::*)({args}){const}{noexcept}"


def free_function_type(cursor) -> str:
    """`Ret (*)(Args...) [noexcept]` - used to disambiguate overloaded free functions."""
    result = cursor.result_type.get_canonical().spelling
    args = ", ".join(a.type.get_canonical().spelling for a in cursor.get_arguments())
    noexcept = " noexcept" if has_noexcept(cursor) else ""
    return f"{result} (*)({args}){noexcept}"


def fill_members(extractor: Extractor, ctx: TypeCtx) -> None:
    for entry in list(extractor.classes.values()):
        cursor = entry["_cursor"]
        cpp_type = entry["cppType"]
        trust = entry.get("_templateInstantiation", False)
        method_counts: Dict[str, int] = {}
        for child in cursor.get_children():
            # Count template methods too: a non-template method that shares a name
            # with a template overload needs an explicit static_cast to disambiguate
            # the address-of expression.
            if child.kind in (CursorKind.CXX_METHOD, CursorKind.FUNCTION_TEMPLATE):
                method_counts[child.spelling] = method_counts.get(child.spelling, 0) + 1

        for child in cursor.get_children():
            if child.access_specifier != AccessSpecifier.PUBLIC:
                continue
            if child.kind == CursorKind.CXX_METHOD:
                if extractor.require_api_macro:
                    if extractor.matches_target(child, cursor):
                        emit_method(extractor, entry, child, cpp_type, ctx, method_counts, trust)
                elif has_skip_macro(child):
                    extractor.skipped.append(f"{cpp_type}::{child.spelling}: marked LLNDAPI/MCNAPI (not bound)")
                else:
                    emit_method(extractor, entry, child, cpp_type, ctx, method_counts, trust)
            elif child.kind == CursorKind.FIELD_DECL:
                if extractor.require_api_macro:
                    continue  # allow-list exports functions/methods only, not data members
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
                if extractor.require_api_macro and not extractor.matches_target(child, cursor):
                    continue
                emit_constructor(extractor, entry, child, cpp_type, ctx, trust)


def resolve_bases(extractor: Extractor) -> None:
    """Resolve each bound class's primary public base (idempotent).

    Sets `base` (scriptName) when the base is bound in the SAME extractor, and
    always records `_baseCppType` (canonical `::`-qualified spelling) so a caller
    with a global view can wire cross-file inheritance through `baseCppType`.
    Template-specialization bases are bound locally by expand_base_instantiations,
    which pre-resolves the specialization's own (substituted) base and marks those
    entries `_baseResolved` so they are skipped here.
    """
    normalized: Dict[str, Dict[str, Any]] = {}
    for key, value in extractor.classes.items():
        normalized[key.lstrip(":")] = value
    for entry in extractor.classes.values():
        if entry.get("_baseResolved"):
            continue
        entry["_baseResolved"] = True
        cursor = entry.get("_cursor")
        if cursor is None:
            continue
        base_child = None
        for child in cursor.get_children():
            if child.kind == CursorKind.CXX_BASE_SPECIFIER and child.access_specifier == AccessSpecifier.PUBLIC:
                base_child = child
                break
        if base_child is None:
            continue
        canon = base_child.type.get_canonical()
        base_spelling = _strip_prefixes(canon.spelling).lstrip(":")
        if not base_spelling or _is_dependent_spec(canon, base_spelling):
            continue
        entry["_baseCppType"] = "::" + base_spelling
        entry.setdefault("_refs", set()).add(base_spelling)
        base_decl = base_child.referenced
        base_qname = qualified_name(base_decl) if base_decl is not None else ""
        target = normalized.get(base_qname) or normalized.get(base_spelling)
        if target is not None and target is not entry:
            entry["base"] = target["scriptName"]
        else:
            entry["_baseUnresolved"] = True


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
    entry.setdefault("_refs", set()).update(r.cpp_type.lstrip(":") for r in referenced_by_function(cursor))


def is_abstract_record(class_cursor) -> bool:
    """True if the class has a pure-virtual method, so `new T()` is ill-formed and no
    script constructor may be synthesized for it."""
    try:
        for child in class_cursor.get_children():
            if child.kind == CursorKind.CXX_METHOD and child.is_pure_virtual_method():
                return True
    except Exception:
        return False
    return False


def _tokens_before_body(cursor) -> List[str]:
    """Token spellings of a declaration up to (not including) the first ';' or '{'."""
    out: List[str] = []
    try:
        for tok in cursor.get_tokens():
            if tok.spelling in (";", "{"):
                break
            out.append(tok.spelling)
    except Exception:
        return []
    return out


def is_deleted(cursor) -> bool:
    """True if the declaration is `= delete`."""
    return "delete" in _tokens_before_body(cursor)


def has_noexcept(cursor) -> bool:
    """True if the declaration carries an (unconditional) `noexcept` specifier."""
    return "noexcept" in _tokens_before_body(cursor)


def is_copy_or_move_ctor(cursor) -> bool:
    """True if the constructor's first parameter is the enclosing class (by value or
    reference): a copy/move constructor, which is not script-constructible (and is
    frequently `= delete`d)."""
    try:
        args = list(cursor.get_arguments())
        if not args:
            return False
        t = args[0].type.get_canonical()
        while t.kind in (TypeKind.LVALUEREFERENCE, TypeKind.RVALUEREFERENCE):
            t = t.get_pointee().get_canonical()
        decl = t.get_declaration()
        parent = cursor.semantic_parent
        return (decl is not None and parent is not None
                and qualified_name(decl) == qualified_name(parent))
    except Exception:
        return False


def emit_constructor(extractor, entry, cursor, cpp_type, ctx, trust) -> None:
    if entry["constructible"]:
        return
    # Under the allow-list policy the caller already gated on the API marker, so the
    # old skip-list (which excludes LLNDAPI) must NOT run here or it would drop
    # LLNDAPI-marked constructors.
    if not extractor.require_api_macro and has_skip_macro(cursor):
        return
    if is_deleted(cursor) or is_copy_or_move_ctor(cursor):
        return  # `= delete`d, or a copy/move constructor
    if not trust and is_abstract_record(entry["_cursor"]):
        return  # cannot instantiate an abstract class from script
    args = list(cursor.get_arguments())
    if trust:
        # Dependent parameter types cannot be reconstructed; only a default
        # constructor is safe to synthesize for a template instantiation.
        if args:
            return
        entry["constructor"] = f"+[]() -> {cpp_type}* {{ return new {cpp_type}(); }}"
        entry["constructible"] = True
        entry["tsConstructorParams"] = []
        entry.setdefault("_refs", set()).update(r.cpp_type.lstrip(":") for r in referenced_by_function(cursor))
        return
    if not all(classify(a.type, ctx, True)[0] for a in args):
        return
    # Name the lambda parameters (a0, a1, ...) and use canonical spellings so nested /
    # unqualified parameter types resolve at the generated file's scope.
    arg_types = ", ".join(f"{a.type.get_canonical().spelling} a{i}" for i, a in enumerate(args))
    forwards = ", ".join(f"std::move(a{i})" for i in range(len(args)))
    factory = (f"+[]({arg_types}) -> {cpp_type}* {{ return new {cpp_type}({forwards}); }}") \
        if args else f"+[]() -> {cpp_type}* {{ return new {cpp_type}(); }}"
    entry["constructor"] = factory
    entry["constructible"] = True
    entry["tsConstructorParams"] = ts_params(cursor, ctx)
    entry.setdefault("_refs", set()).update(r.cpp_type.lstrip(":") for r in referenced_by_function(cursor))


def build_spec(extractor: Extractor, ctx: TypeCtx, config: Dict[str, Any]) -> Dict[str, Any]:
    # Bind template-specialization bases locally and resolve every base edge before
    # members are filled, so `_baseCppType` is available to the caller's global pass.
    extractor.expand_base_instantiations()
    resolve_bases(extractor)
    fill_members(extractor, ctx)

    # A class referenced by a bound member must be emitted (registered +
    # LS_NATIVE_CLASS) even if it has no API-marked members of its own - e.g. a
    # plain data struct returned via std::optional<T> - otherwise the reference
    # will not compile. `keepTypes` additionally forces base classes to survive
    # pruning so cross-file inheritance can register against them.
    referenced: Set[str] = set()
    if extractor.require_api_macro:
        for entry in extractor.classes.values():
            if entry["methods"] or entry["staticMethods"] or entry["constructible"]:
                for ref in referenced_classes(entry["_cursor"]):
                    referenced.add(ref.cpp_type.lstrip(":"))
                    referenced.add(ref.qname)
        for fn in extractor.functions:
            for ref in referenced_by_function(fn["_cursor"]):
                referenced.add(ref.cpp_type.lstrip(":"))
                referenced.add(ref.qname)
    keep: Set[str] = set(referenced)
    for base_type in config.get("keepTypes", ()):
        keep.add(base_type.lstrip(":"))

    classes: List[Dict[str, Any]] = []
    for entry in extractor.classes.values():
        key = entry["cppType"].lstrip(":")
        is_empty = not (entry["methods"] or entry["staticMethods"] or entry["properties"] or entry["constructible"])
        if (extractor.require_api_macro and is_empty
                and key not in keep and entry.get("_qname") not in keep
                and entry.get("_canonKey") not in keep):
            continue  # no exported members, not referenced, not a base -> nothing to emit
        classes.append({
            "scriptName": entry["scriptName"],
            "cppType": entry["cppType"],
            "base": entry["base"],
            "baseCppType": None,                 # filled by the caller for cross-file bases
            "_baseCppType": entry.get("_baseCppType"),
            "_canonKey": entry.get("_canonKey", key),
            "_header": entry.get("_header"),
            "_refs": sorted(entry.get("_refs", ())),
            "constructible": entry["constructible"],
            **({"constructor": entry["constructor"]} if entry.get("constructor") else {}),
            **({"tsConstructorParams": entry["tsConstructorParams"]} if entry.get("tsConstructorParams") is not None else {}),
            "methods": entry["methods"],
            "properties": entry["properties"],
            "staticMethods": entry["staticMethods"],
        })

    functions: List[Dict[str, Any]] = []
    fn_counts: Dict[str, int] = {}
    for fn in extractor.functions:
        fn_counts[fn["_qname"]] = fn_counts.get(fn["_qname"], 0) + 1
    for fn in extractor.functions:
        cursor = fn["_cursor"]
        ok, reason = signature_bindable(cursor, ctx)
        if not ok:
            extractor.skipped.append(f"function '{fn['_qname']}': {reason}")
            continue
        cpp = fn["_cpp"]
        # Always disambiguate with an explicit cast: a free function may share its
        # name with a template overload (which is not collected), making a bare
        # `&fn` ambiguous, or be genuinely overloaded.
        cpp = f"static_cast<{free_function_type(cursor)}>({cpp})"
        functions.append({"scriptName": cursor.spelling, "cpp": cpp,
                          "tsParams": ts_params(cursor, ctx), "tsReturns": ts_type(cursor.result_type, ctx),
                          "_header": fn.get("_header"),
                          "_refs": sorted(r.cpp_type.lstrip(":") for r in referenced_by_function(cursor))})
    for tf in extractor.template_functions:
        functions.append({"scriptName": tf["scriptName"], "cpp": tf["cpp"]})

    # Include the defining header of every bound class: the closure may pull in
    # classes from other headers, which must be #included so the types are complete
    # at compile time. Merged with (and deduped against) the config's emitIncludes.
    includes = list(config.get("emitIncludes", []))
    for entry in extractor.classes.values():
        header = entry.get("_header")
        if header and header not in includes:
            includes.append(header)

    return {
        "namespace": config.get("scriptNamespace", "native"),
        "functionName": config.get("functionName"),
        "includes": includes,
        "classes": classes,
        "functions": functions,
        "extraNativeClasses": [],   # cross-file base types needing LS_NATIVE_CLASS here
    }


# --------------------------------------------------------------------------- #
# driver
# --------------------------------------------------------------------------- #
def build_parse_args(config: Dict[str, Any]) -> List[str]:
    args = ["-x", "c++", "-std=" + config.get("std", "c++20"),
            "-fms-extensions", "-fms-compatibility", "--target=x86_64-pc-windows-msvc",
            "-D_WIN32", "-DWIN32", "-D_CRT_SECURE_NO_WARNINGS"]
    for define in config.get("defines", []):
        args.append("-D" + define)
    for inc in config.get("includeDirs", []):
        args.append("-I" + inc)
    return args


def parse_one(index, header: str, args: List[str]):
    tu = index.parse(header, args=args, options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
    diags = [d for d in tu.diagnostics if d.severity >= cindex.Diagnostic.Error]
    if diags:
        print(f"warning: {header}: {len(diags)} error diagnostic(s) while parsing "
              f"(first: {diags[0].spelling})", file=sys.stderr)
    return tu


class HeaderResolver:
    """Maps a class leaf name to candidate `<leaf>.h` headers under the include dirs.

    LL/MC name headers after their primary class, so this heuristic resolves most
    forward-declared references encountered during closure. The directory index is
    built lazily (walking the include tree can be large) and only when a cross-header
    lookup is actually needed.
    """

    def __init__(self, include_dirs: List[str]):
        self.include_dirs = include_dirs
        self._index: Optional[Dict[str, List[str]]] = None

    def _build(self) -> None:
        index: Dict[str, List[str]] = {}
        for directory in self.include_dirs:
            if not os.path.isdir(directory):
                continue
            for root, _dirs, files in os.walk(directory):
                for name in files:
                    if name.endswith((".h", ".hpp", ".hh", ".hxx")):
                        index.setdefault(name.rsplit(".", 1)[0], []).append(os.path.join(root, name))
        self._index = index

    def candidates(self, leaf: str) -> List[str]:
        if self._index is None:
            self._build()
        return self._index.get(leaf, [])


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
    index = cindex.Index.create()
    parse_args = build_parse_args(config)

    extractor = Extractor(config, target_files)
    parsed_paths: Set[str] = set()
    for header in config["headers"]:
        tu = parse_one(index, header, parse_args)
        parsed_paths.add(os.path.normcase(os.path.abspath(header)))
        extractor.run(tu)                # seed: classes/functions located in the target files
        extractor.index_definitions(tu)  # index every definition for closure resolution

    if config.get("closure"):
        resolver = HeaderResolver(config.get("includeDirs", []))

        def resolve_header(qname: str, leaf: str):
            # Locate `<leaf>.h`, parse it, index it, and return the class definition.
            for path in resolver.candidates(leaf):
                norm = os.path.normcase(os.path.abspath(path))
                if norm in parsed_paths:
                    continue
                extra_tu = parse_one(index, path, parse_args)
                parsed_paths.add(norm)
                extractor.index_definitions(extra_tu)
                if qname in extractor.definitions:
                    return extractor.definitions[qname]
            return None

        seed_count = len(extractor.classes)
        extractor.expand_closure(resolve_header)
        print(f"closure: {seed_count} seed -> {len(extractor.classes)} bound classes")

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
