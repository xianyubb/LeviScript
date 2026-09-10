#!/usr/bin/env python3
"""Batch driver: generate one binding file per configured real header.

Iterates every tools/autobind/config/*.json (skipping *.spec.json / *.report.json)
and runs parse_headers.py for each, emitting:

    src/native/generated/<Header>.cpp        compiled into the mod, wired in bindApis
    types/generated/<Header>.d.ts            TypeScript declaration for editors
    tools/autobind/config/<cfg>.report.json  what was skipped and why

The output base name mirrors the source header path so that each binding file
corresponds to exactly one header:

    .../include/ll/api/data/Version.h  ->  ll_api_data_Version.cpp

Adding another real header is therefore: drop a new config JSON into config/
(one header per config), re-run this script, and call the emitted
`bindGenerated...` function from ls::native::bindApis (src/native/NativeModule.cpp).

Usage:
    python tools/autobind/generate_all.py
"""

from __future__ import annotations

import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def header_base(cfg: dict) -> str | None:
    """Derive an output base name from the config's (single) header path."""
    headers = cfg.get("headers") or []
    if not headers:
        return None
    header = headers[0].replace("\\", "/")
    match = re.search(r"include/(.+)$", header)
    rel = match.group(1) if match else os.path.basename(header)
    if rel.endswith(".h") or rel.endswith(".hpp"):
        rel = rel.rsplit(".", 1)[0]
    return re.sub(r"[^A-Za-z0-9]+", "_", rel)


def main() -> int:
    configs = sorted(glob.glob(os.path.join(HERE, "config", "*.json")))
    configs = [c for c in configs
               if not os.path.basename(c).endswith((".spec.json", ".report.json"))]
    if not configs:
        print("no configs found in tools/autobind/config", file=sys.stderr)
        return 1

    for cfg_path in configs:
        with open(cfg_path, "r", encoding="utf-8-sig") as handle:
            cfg = json.load(handle)
        stem = os.path.splitext(os.path.basename(cfg_path))[0]
        report = os.path.join(HERE, "config", stem + ".report.json")
        parse = os.path.join(HERE, "parse_headers.py")

        # A probe config ("specOnly": true) is parsed for its spec + skip report but
        # emits no .cpp, so an experimental/real header that is not yet wireable
        # (e.g. mc value types) never enters the mod build.
        if cfg.get("specOnly"):
            spec = os.path.join(HERE, "config", stem + ".spec.json")
            cmd = [sys.executable, parse, cfg_path, "--spec", spec, "--report", report]
            print(f">> {stem}: spec-only probe -> {os.path.relpath(report, ROOT)}")
            subprocess.run(cmd, cwd=ROOT, check=True)
            continue

        base = header_base(cfg)
        if base is None:
            print(f"skip (no headers): {cfg_path}", file=sys.stderr)
            continue
        cpp = os.path.join(ROOT, "src", "native", "generated", base + ".cpp")
        dts = os.path.join(ROOT, "types", "generated", base + ".d.ts")
        os.makedirs(os.path.dirname(cpp), exist_ok=True)
        os.makedirs(os.path.dirname(dts), exist_ok=True)
        cmd = [sys.executable, parse, cfg_path, "--cpp", cpp, "--dts", dts, "--report", report]
        print(f">> {stem}: {base} -> {os.path.relpath(cpp, ROOT)}")
        subprocess.run(cmd, cwd=ROOT, check=True)

    print("batch generation complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
