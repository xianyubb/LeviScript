#!/usr/bin/env python3
"""LeviScript API validator.

This is the "validation API program": it verifies that the API actually present
in a running engine matches what the binding spec promises.

Workflow
--------
1. Drop the plugin in tools/apivalidator/dump_plugin into the server's plugins
   directory and start the server once. It calls `ll.dumpApiSurface("api_surface.json")`
   which writes the live API surface next to the plugin.
2. Produce an expectation, either by hand or straight from a binding spec:
       python ../autobind/autobind.py spec.json -o out.cpp --emit-expected expected.json
3. Validate:
       python validate.py --surface api_surface.json --expected expected.json

The check is a recursive *subset* test: everything listed in the expectation must
exist in the surface with a compatible shape. Extra entries in the surface are
allowed (reported only with --verbose). Exit code is non-zero when anything
required is missing or has the wrong type, so it can gate CI.

Surface / expected node shape (as produced by exportApiSurface):
    {"type": "function"}
    {"type": "class", "methods": ["a", "b"]}
    {"type": "object", "members": { ... nested nodes ... }}
    {"type": "number" | "string" | "boolean" | ...}
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any, Dict, List, Tuple


class Report:
    def __init__(self) -> None:
        self.errors: List[str] = []
        self.infos: List[str] = []

    def error(self, message: str) -> None:
        self.errors.append(message)

    def info(self, message: str) -> None:
        self.infos.append(message)


def node_type(node: Any) -> str:
    if isinstance(node, dict):
        return str(node.get("type", "object"))
    return "unknown"


def check_node(path: str, expected: Any, surface: Any, report: Report) -> None:
    exp_type = node_type(expected)

    if surface is None:
        report.error(f"{path}: missing (expected {exp_type})")
        return

    surf_type = node_type(surface)

    # A class is also a function; tolerate that equivalence.
    compatible = (exp_type == surf_type) or (exp_type == "class" and surf_type == "function") \
        or (exp_type == "function" and surf_type == "class")
    if not compatible:
        report.error(f"{path}: type mismatch (expected '{exp_type}', found '{surf_type}')")
        return

    if exp_type == "class":
        for bucket in ("methods", "staticMethods"):
            expected_members = set(expected.get(bucket, []))
            surface_members = set(surface.get(bucket, []))
            missing = sorted(expected_members - surface_members)
            if missing:
                report.error(f"{path}: missing {bucket} {missing}")
            elif expected_members:
                report.info(f"{path}: {bucket} OK ({len(expected_members)} present)")

    elif exp_type == "object":
        expected_members: Dict[str, Any] = expected.get("members", {})
        surface_members: Dict[str, Any] = surface.get("members", {})
        for key, sub_expected in expected_members.items():
            check_node(f"{path}.{key}", sub_expected, surface_members.get(key), report)
        for key in surface_members:
            if key not in expected_members:
                report.info(f"{path}.{key}: present but not required")


def validate(surface: Dict[str, Any], expected: Dict[str, Any]) -> Report:
    report = Report()
    for key, sub_expected in expected.items():
        check_node(key, sub_expected, surface.get(key), report)
    return report


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Validate the LeviScript runtime API surface against a spec")
    parser.add_argument("--surface", required=True, help="api_surface.json dumped from a running engine")
    parser.add_argument("--expected", required=True, help="expected API surface JSON (subset)")
    parser.add_argument("--verbose", action="store_true", help="also print informational notes")
    args = parser.parse_args(argv)

    try:
        with open(args.surface, "r", encoding="utf-8-sig") as handle:
            surface = json.load(handle)
        with open(args.expected, "r", encoding="utf-8-sig") as handle:
            expected = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    report = validate(surface, expected)

    if args.verbose:
        for line in report.infos:
            print(f"[info] {line}")
    for line in report.errors:
        print(f"[FAIL] {line}")

    if report.errors:
        print(f"\n{len(report.errors)} problem(s) found.", file=sys.stderr)
        return 1
    print("API surface matches the expected specification.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
