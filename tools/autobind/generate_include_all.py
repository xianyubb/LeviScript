#!/usr/bin/env python3
"""Generate a single umbrella header that includes every ll/ and mc/ header.

The script walks the LeviLamina package include directory (the folder that holds
the `ll/` and `mc/` header trees), enumerates every `.h` / `.hpp` under the
requested roots and emits `src/plugin/include_all.h` in the same style as the
existing `include_ll.h` / `include_mc.h`:

    // This header file is automatically generated. Do not modify it manually
    #pragma once
    // IWYU pragma: begin_exports
    #include "ll/api/base/Alias.h"
    ...
    #include "mc/_HeaderOutputPredefine.h"
    #include "mc/cereal/..."
    ...
    // IWYU pragma: end_exports

Include paths are written relative to the include root with forward slashes, so
they resolve through the `levilamina` package include dir already on the build's
search path. `mc/_HeaderOutputPredefine.h` (which defines MCAPI and pulls in the
STL) is hoisted to the top of the mc block; every other header is self-contained
via `#pragma once`, so the remaining order is a plain case-insensitive sort.

The include root is resolved in this order:
  1. --include-root PATH           (explicit)
  2. --config PATH                 (default: tools/autobind/config/tree_ll_api.json,
                                    reads the includeDirs / headerRoot from it)
  3. auto-detect                   (scan the local .xmake levilamina packages)

Usage:
    python tools/autobind/generate_include_all.py
    python tools/autobind/generate_include_all.py --include-root "<...>/include"
    python tools/autobind/generate_include_all.py --comment-root mc   # keep mc disabled
"""

from __future__ import annotations

import argparse
import fnmatch
import glob
import json
import os
import sys
from typing import Dict, List, Optional, Sequence

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_CONFIG = os.path.join(HERE, "config", "tree_ll_api.json")
DEFAULT_OUTPUT = os.path.join(ROOT, "src", "plugin", "include_all.h")
HEADER_EXTS = (".h", ".hpp")


# --------------------------------------------------------------------------- #
# include-root resolution
# --------------------------------------------------------------------------- #
def _has_roots(directory: str, roots: Sequence[str]) -> bool:
    return all(os.path.isdir(os.path.join(directory, r)) for r in roots)


def _normalize(path: str) -> str:
    return os.path.abspath(path).replace("\\", "/")


def root_from_config(cfg_path: str, roots: Sequence[str]) -> Optional[str]:
    """Pick the include dir (from an autobind tree config) that holds every root."""
    try:
        with open(cfg_path, "r", encoding="utf-8-sig") as handle:
            cfg = json.load(handle)
    except (OSError, ValueError) as exc:
        print(f"  warning: cannot read config {cfg_path}: {exc}", file=sys.stderr)
        return None

    candidates: List[str] = [str(p) for p in (cfg.get("includeDirs") or [])]
    header_root = cfg.get("headerRoot")
    if header_root:  # walk up from e.g. .../include/ll/api to .../include
        cur = str(header_root)
        for _ in range(6):
            parent = os.path.dirname(cur)
            if not parent or parent == cur:
                break
            candidates.append(parent)
            cur = parent
    for cand in candidates:
        cand = _normalize(cand)
        if _has_roots(cand, roots):
            return cand
    return None


def root_from_xmake(roots: Sequence[str]) -> Optional[str]:
    """Auto-detect the newest installed levilamina include dir under .xmake."""
    bases: List[str] = []
    local = os.environ.get("LOCALAPPDATA")
    if local:
        bases.append(os.path.join(local, ".xmake", "packages"))
    bases.append(os.path.join(os.path.expanduser("~"), ".xmake", "packages"))

    best: Optional[str] = None
    best_mtime = -1.0
    for base in bases:
        pattern = os.path.join(base, "l", "levilamina", "*", "*", "include")
        for inc in glob.glob(pattern):
            inc = _normalize(inc)
            if not _has_roots(inc, roots):
                continue
            mtime = os.path.getmtime(inc)
            if mtime > best_mtime:
                best, best_mtime = inc, mtime
    return best


def resolve_include_root(args: argparse.Namespace, roots: Sequence[str]) -> str:
    if args.include_root:
        root = _normalize(args.include_root)
        if not _has_roots(root, roots):
            raise SystemExit(f"error: --include-root {root} has no {'/'.join(roots)} subdir(s)")
        print(f"include root (explicit): {root}")
        return root

    cfg_path = args.config or DEFAULT_CONFIG
    if cfg_path and os.path.exists(cfg_path):
        root = root_from_config(cfg_path, roots)
        if root:
            print(f"include root (from {os.path.relpath(cfg_path, ROOT)}): {root}")
            return root

    root = root_from_xmake(roots)
    if root:
        print(f"include root (auto-detected): {root}")
        return root

    raise SystemExit(
        "error: could not locate the LeviLamina include dir. "
        "Pass --include-root <...>/include or fix --config."
    )


# --------------------------------------------------------------------------- #
# header enumeration
# --------------------------------------------------------------------------- #
def _sort_key(rel: str) -> tuple:
    """Hoist `_Header...Predefine.h` first, then plain case-insensitive path order."""
    base = rel.rsplit("/", 1)[-1]
    is_predefine = base.startswith("_") and "predefine" in base.lower()
    return (0 if is_predefine else 1, rel.lower())


def collect_headers(include_root: str, roots: Sequence[str], excludes: Sequence[str]) -> Dict[str, List[str]]:
    """Map each root -> sorted list of include-relative header paths."""
    grouped: Dict[str, List[str]] = {}
    for root in roots:
        base = os.path.join(include_root, root)
        if not os.path.isdir(base):
            grouped[root] = []
            continue
        found: List[str] = []
        for dirpath, _dirnames, filenames in os.walk(base):
            for name in filenames:
                if not name.endswith(HEADER_EXTS):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), include_root).replace(os.sep, "/")
                if any(fnmatch.fnmatch(rel, pat) for pat in excludes):
                    continue
                found.append(rel)
        found.sort(key=_sort_key)
        grouped[root] = found
    return grouped


# --------------------------------------------------------------------------- #
# emit
# --------------------------------------------------------------------------- #
def render(grouped: Dict[str, List[str]], roots: Sequence[str], comment_roots: Sequence[str]) -> str:
    commented = {r for r in comment_roots}
    lines: List[str] = [
        "// This header file is automatically generated. Do not modify it manually",
        "// This header file is automatically generated. Do not modify it manually",
        "// This header file is automatically generated. Do not modify it manually",
        "// Generated by tools/autobind/generate_include_all.py",
    ]
    for root in roots:
        lines.append(f"//   {root}: {len(grouped.get(root, []))} header(s)")
    lines += ["", "#pragma once", "", "// IWYU pragma: begin_exports"]
    for root in roots:
        items = grouped.get(root) or []
        if not items:
            continue
        lines.append(f"// ---- {root} ----")
        prefix = "// " if root in commented else ""
        for rel in items:
            lines.append(f'{prefix}#include "{rel}"')
    lines += ["// IWYU pragma: end_exports", ""]
    return "\n".join(lines)


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Generate include_all.h covering every ll/ and mc/ header")
    ap.add_argument("--include-root", help="LeviLamina include dir that holds the ll/ and mc/ trees")
    ap.add_argument("--config", help=f"autobind tree config to read the include dir from (default: {os.path.relpath(DEFAULT_CONFIG, ROOT)})")
    ap.add_argument("-o", "--output", default=DEFAULT_OUTPUT, help="output header (default: src/plugin/include_all.h)")
    ap.add_argument("--roots", nargs="+", default=["ll", "mc"], help="top-level header folders to include (default: ll mc)")
    ap.add_argument("--exclude", action="append", default=[], metavar="GLOB",
                    help="skip include-relative paths matching this glob (repeatable)")
    ap.add_argument("--comment-root", action="append", default=[], metavar="ROOT",
                    help="emit this root's includes commented out, e.g. --comment-root mc (repeatable)")
    args = ap.parse_args(argv)

    include_root = resolve_include_root(args, args.roots)
    grouped = collect_headers(include_root, args.roots, args.exclude)

    total = sum(len(v) for v in grouped.values())
    if total == 0:
        raise SystemExit(f"error: no headers found under {include_root} for roots {args.roots}")

    text = render(grouped, args.roots, args.comment_root)
    out_path = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)

    for root in args.roots:
        note = " (commented)" if root in set(args.comment_root) else ""
        print(f"  {root}: {len(grouped.get(root, []))} header(s){note}")
    print(f"wrote {total} include(s) -> {os.path.relpath(out_path, ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
