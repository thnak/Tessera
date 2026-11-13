#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read tests/golden/tolerance.json for the golden-test wiring (issue #161).

By default this prints one ``name<TAB>tolerance<TAB>max_mismatch`` line per golden test, sorted by
name. ``--json`` prints the same information as a canonical JSON document instead, and
``--name <test> [--field tolerance|max_mismatch]`` lets CMake read a single value for a single
test. An unknown test name resolves to the ``defaults`` block, which is what makes the file safe
to extend without touching every registration.

Schema (``schema`` is the only version so far):

    {
      "schema": 1,
      "defaults": {"tolerance": 0, "max_mismatch": 0.0},
      "tests": {"<golden ctest name>": {"tolerance": 1, "max_mismatch": 0.001}}
    }

``tolerance`` is the per-channel 8-bit delta a pixel may have and still count as matching;
``max_mismatch`` is the fraction of pixels (0.0-1.0) allowed to exceed it. Validation is strict so
a typo fails the build instead of silently changing what a golden accepts.

Self-tests: ``python -B -m unittest discover -s scripts/tests -p "test_golden_tolerances.py" -v``.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
DEFAULT_TOLERANCE = 0
DEFAULT_MAX_MISMATCH = 0.0
TOP_LEVEL_KEYS = {"schema", "defaults", "tests"}
ENTRY_KEYS = {"tolerance", "max_mismatch"}

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PATH = REPO_ROOT / "tests" / "golden" / "tolerance.json"


class ToleranceError(ValueError):
    """tolerance.json is unreadable, malformed, or uses an unknown schema."""


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _validate_tolerance(value: Any, where: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ToleranceError(f"{where}: tolerance must be an integer")
    if value < 0:
        raise ToleranceError(f"{where}: tolerance must be >= 0")
    return value


def _validate_max_mismatch(value: Any, where: str) -> float:
    if not _is_number(value):
        raise ToleranceError(f"{where}: max_mismatch must be a number")
    number = float(value)
    if not math.isfinite(number):
        raise ToleranceError(f"{where}: max_mismatch must be finite")
    if number < 0.0 or number > 1.0:
        raise ToleranceError(f"{where}: max_mismatch must be a fraction between 0 and 1")
    return number


def _validate_name(name: Any, where: str) -> str:
    if not isinstance(name, str) or not name:
        raise ToleranceError(f"{where}: a test name must be a non-empty string")
    if any(character.isspace() for character in name):
        raise ToleranceError(f"{where}: test name {name!r} contains whitespace")
    return name


def _validate_entry(entry: Any, where: str) -> dict[str, Any]:
    if not isinstance(entry, dict):
        raise ToleranceError(f"{where}: expected an object")
    unknown = set(entry) - ENTRY_KEYS
    if unknown:
        raise ToleranceError(f"{where}: unknown key(s): {', '.join(sorted(unknown))}")
    missing = ENTRY_KEYS - set(entry)
    if missing:
        raise ToleranceError(f"{where}: missing key(s): {', '.join(sorted(missing))}")
    return {
        "tolerance": _validate_tolerance(entry["tolerance"], f"{where}.tolerance"),
        "max_mismatch": _validate_max_mismatch(entry["max_mismatch"], f"{where}.max_mismatch"),
    }


def validate(document: Any) -> dict[str, Any]:
    """Return a normalized copy of ``document`` or raise ToleranceError."""

    if not isinstance(document, dict):
        raise ToleranceError("the document root must be an object")
    unknown = set(document) - TOP_LEVEL_KEYS
    if unknown:
        raise ToleranceError(f"unknown top-level key(s): {', '.join(sorted(unknown))}")
    missing = TOP_LEVEL_KEYS - set(document)
    if missing:
        raise ToleranceError(f"missing top-level key(s): {', '.join(sorted(missing))}")
    schema = document["schema"]
    if not isinstance(schema, int) or isinstance(schema, bool) or schema != SCHEMA_VERSION:
        raise ToleranceError(f"schema must be {SCHEMA_VERSION}")

    tests_document = document["tests"]
    if not isinstance(tests_document, dict):
        raise ToleranceError("tests must be an object keyed by golden test name")
    tests: dict[str, dict[str, Any]] = {}
    for name, entry in tests_document.items():
        validated_name = _validate_name(name, "tests key")
        tests[validated_name] = _validate_entry(entry, f"tests.{validated_name}")

    return {
        "schema": SCHEMA_VERSION,
        "defaults": _validate_entry(document["defaults"], "defaults"),
        "tests": tests,
    }


def load(path: Path | None = None) -> dict[str, Any]:
    """Load and validate tolerance.json (the checked-in file by default)."""

    target = Path(path) if path is not None else DEFAULT_PATH
    try:
        text = target.read_text(encoding="utf-8")
    except OSError as error:
        raise ToleranceError(f"cannot read {target}: {error}") from error
    try:
        document = json.loads(text)
    except json.JSONDecodeError as error:
        raise ToleranceError(f"{target}: invalid JSON: {error}") from error
    return validate(document)


def resolve(document: dict[str, Any], name: str) -> tuple[int, float]:
    """The (tolerance, max_mismatch) a golden test runs with; unknown names use defaults."""

    entry = document["tests"].get(name, document["defaults"])
    return int(entry["tolerance"]), float(entry["max_mismatch"])


def format_number(value: float) -> str:
    """Shortest stable text for a float: 0.0 -> "0", 0.001 -> "0.001"."""

    if value == int(value):
        return str(int(value))
    return repr(value)


def entries(document: dict[str, Any]) -> list[tuple[str, int, float]]:
    """Every explicit entry, sorted by test name (deterministic text output)."""

    rows = []
    for name in sorted(document["tests"]):
        tolerance, max_mismatch = resolve(document, name)
        rows.append((name, tolerance, max_mismatch))
    return rows


def format_text(document: dict[str, Any]) -> str:
    lines = [
        f"{name}\t{tolerance}\t{format_number(max_mismatch)}"
        for name, tolerance, max_mismatch in entries(document)
    ]
    return "".join(line + "\n" for line in lines)


def format_json(document: dict[str, Any]) -> str:
    payload = {
        "schema": document["schema"],
        "defaults": document["defaults"],
        "tests": [
            {"name": name, "tolerance": tolerance, "max_mismatch": max_mismatch}
            for name, tolerance, max_mismatch in entries(document)
        ],
    }
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Read tests/golden/tolerance.json (issue #161).",
    )
    parser.add_argument(
        "--path",
        type=Path,
        default=DEFAULT_PATH,
        help=f"tolerance file to read (default: {DEFAULT_PATH})",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="print the resolved document as canonical JSON instead of tab-separated lines",
    )
    parser.add_argument(
        "--name",
        help="print only this golden test (an unknown name resolves to the defaults)",
    )
    parser.add_argument(
        "--field",
        choices=("tolerance", "max_mismatch"),
        help="with --name, print only that field's value",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.field is not None and args.name is None:
        parser.error("--field requires --name")
    if args.json and args.name is not None:
        parser.error("--json cannot be combined with --name")

    try:
        document = load(args.path)
    except ToleranceError as error:
        print(f"golden_tolerances: {error}", file=sys.stderr)
        return 1

    if args.json:
        sys.stdout.write(format_json(document))
        return 0

    if args.name is not None:
        tolerance, max_mismatch = resolve(document, args.name)
        if args.field == "tolerance":
            print(tolerance)
        elif args.field == "max_mismatch":
            print(format_number(max_mismatch))
        else:
            print(f"{args.name}\t{tolerance}\t{format_number(max_mismatch)}")
        return 0

    sys.stdout.write(format_text(document))
    return 0


if __name__ == "__main__":
    sys.exit(main())
