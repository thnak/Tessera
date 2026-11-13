#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read tests/golden/tolerance.json for the golden-test wiring (issue #161, extended by #245).

By default this prints one ``name<TAB>tolerance<TAB>max_mismatch`` line per golden test, sorted by
name. ``--json`` prints the same information as a canonical JSON document instead, and
``--name <test> [--field tolerance|max_mismatch]`` lets CMake read a single value for a single
test. An unknown test name resolves to the ``defaults`` block, which is what makes the file safe
to extend without touching every registration.

Schema 2 (issue #245) adds an optional per-test ``pairs`` block, keyed by ``<backend>_vs_
<reference>``, for cross-backend image parity. A pair that is not listed is exact:
``tolerance`` 0, ``max_mismatch`` 0.0, ``ssim`` 1.0, ``delta_e`` 0.0. ``--pair`` prints one pair's
values, and ``--pairs-header <path>`` writes the C++ table the parity harness compiles in.

Schemas 1 and 2 are accepted: schema 1 documents (and entries) have no ``pairs`` block and stay
valid unchanged.

    {
      "schema": 2,
      "defaults": {"tolerance": 0, "max_mismatch": 0.0},
      "tests": {
        "<golden ctest name>": {
          "tolerance": 1,
          "max_mismatch": 0.001,
          "pairs": {
            "<candidate>_vs_<reference>": {
              "tolerance": 8,
              "max_mismatch": 0.01,
              "ssim": 0.99,
              "delta_e": 2.0
            }
          }
        }
      }
    }

``tolerance`` is the per-channel 8-bit delta a pixel may have and still count as matching (and,
inside a pair, the largest delta that may be classified as edge rounding); ``max_mismatch`` is the
fraction of pixels (0.0-1.0) allowed to exceed it; ``ssim`` is the floor for the mean SSIM on luma
and ``delta_e`` the ceiling for the mean CIE76 deltaE. Validation is strict so a typo fails the
build instead of silently changing what a golden accepts.

Self-tests: ``python -B -m unittest discover -s scripts/tests -p "test_golden_tolerances.py" -v``.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 2
SUPPORTED_SCHEMAS = (1, 2)
DEFAULT_TOLERANCE = 0
DEFAULT_MAX_MISMATCH = 0.0
DEFAULT_SSIM = 1.0
DEFAULT_DELTA_E = 0.0
TOP_LEVEL_KEYS = {"schema", "defaults", "tests"}
ENTRY_KEYS = {"tolerance", "max_mismatch", "pairs"}
PAIR_KEYS = {"tolerance", "max_mismatch", "ssim", "delta_e"}
PAIR_NAME_CHARACTERS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789_")

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PATH = REPO_ROOT / "tests" / "golden" / "tolerance.json"

# The exact policy every unlisted pair resolves to (issue #245).
EXACT_PAIR: dict[str, Any] = {
    "tolerance": DEFAULT_TOLERANCE,
    "max_mismatch": DEFAULT_MAX_MISMATCH,
    "ssim": DEFAULT_SSIM,
    "delta_e": DEFAULT_DELTA_E,
}


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


def _validate_ssim(value: Any, where: str) -> float:
    if not _is_number(value):
        raise ToleranceError(f"{where}: ssim must be a number")
    number = float(value)
    if not math.isfinite(number):
        raise ToleranceError(f"{where}: ssim must be finite")
    if number < 0.0 or number > 1.0:
        raise ToleranceError(f"{where}: ssim must be between 0 and 1")
    return number


def _validate_delta_e(value: Any, where: str) -> float:
    if not _is_number(value):
        raise ToleranceError(f"{where}: delta_e must be a number")
    number = float(value)
    if not math.isfinite(number):
        raise ToleranceError(f"{where}: delta_e must be finite")
    if number < 0.0:
        raise ToleranceError(f"{where}: delta_e must be >= 0")
    return number


def _validate_name(name: Any, where: str) -> str:
    if not isinstance(name, str) or not name:
        raise ToleranceError(f"{where}: a test name must be a non-empty string")
    if any(character.isspace() for character in name):
        raise ToleranceError(f"{where}: test name {name!r} contains whitespace")
    return name


def _validate_pair_name(name: Any, where: str) -> str:
    if not isinstance(name, str) or not name:
        raise ToleranceError(f"{where}: a pair name must be a non-empty string")
    unknown = set(name) - PAIR_NAME_CHARACTERS
    if unknown:
        raise ToleranceError(
            f"{where}: pair name {name!r} may only use lowercase letters, digits and '_'"
        )
    if "_vs_" not in name:
        raise ToleranceError(f"{where}: pair name {name!r} must look like <candidate>_vs_<reference>")
    return name


def _validate_pair(entry: Any, where: str) -> dict[str, Any]:
    if not isinstance(entry, dict):
        raise ToleranceError(f"{where}: expected an object")
    unknown = set(entry) - PAIR_KEYS
    if unknown:
        raise ToleranceError(f"{where}: unknown key(s): {', '.join(sorted(unknown))}")
    missing = PAIR_KEYS - set(entry)
    if missing:
        raise ToleranceError(f"{where}: missing key(s): {', '.join(sorted(missing))}")
    return {
        "tolerance": _validate_tolerance(entry["tolerance"], f"{where}.tolerance"),
        "max_mismatch": _validate_max_mismatch(entry["max_mismatch"], f"{where}.max_mismatch"),
        "ssim": _validate_ssim(entry["ssim"], f"{where}.ssim"),
        "delta_e": _validate_delta_e(entry["delta_e"], f"{where}.delta_e"),
    }


def _validate_entry(entry: Any, where: str, schema: int) -> dict[str, Any]:
    if not isinstance(entry, dict):
        raise ToleranceError(f"{where}: expected an object")
    unknown = set(entry) - ENTRY_KEYS
    if unknown:
        raise ToleranceError(f"{where}: unknown key(s): {', '.join(sorted(unknown))}")
    required = ENTRY_KEYS - {"pairs"}
    missing = required - set(entry)
    if missing:
        raise ToleranceError(f"{where}: missing key(s): {', '.join(sorted(missing))}")

    pairs_document = entry.get("pairs", {})
    if not isinstance(pairs_document, dict):
        raise ToleranceError(f"{where}.pairs: expected an object keyed by pair name")
    if pairs_document and schema < 2:
        raise ToleranceError(f"{where}.pairs: per-backend-pair tolerances need schema 2")
    pairs: dict[str, dict[str, Any]] = {}
    for name, pair_entry in pairs_document.items():
        validated_name = _validate_pair_name(name, f"{where}.pairs key")
        pairs[validated_name] = _validate_pair(pair_entry, f"{where}.pairs.{validated_name}")

    return {
        "tolerance": _validate_tolerance(entry["tolerance"], f"{where}.tolerance"),
        "max_mismatch": _validate_max_mismatch(entry["max_mismatch"], f"{where}.max_mismatch"),
        "pairs": pairs,
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
    if not isinstance(schema, int) or isinstance(schema, bool) or schema not in SUPPORTED_SCHEMAS:
        raise ToleranceError(f"schema must be one of {', '.join(str(item) for item in SUPPORTED_SCHEMAS)}")

    tests_document = document["tests"]
    if not isinstance(tests_document, dict):
        raise ToleranceError("tests must be an object keyed by golden test name")
    tests: dict[str, dict[str, Any]] = {}
    for name, entry in tests_document.items():
        validated_name = _validate_name(name, "tests key")
        tests[validated_name] = _validate_entry(entry, f"tests.{validated_name}", schema)

    defaults = _validate_entry(document["defaults"], "defaults", schema)
    if defaults["pairs"]:
        raise ToleranceError("defaults.pairs: per-pair tolerances belong to a test entry")

    return {
        "schema": schema,
        "defaults": defaults,
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


def resolve_pair(document: dict[str, Any], name: str, pair: str) -> tuple[int, float, float, float]:
    """The pair policy for (test, pair); an unlisted pair is exact."""

    entry = document["tests"].get(name)
    pairs = entry["pairs"] if entry else {}
    resolved = pairs.get(pair, EXACT_PAIR)
    return (
        int(resolved["tolerance"]),
        float(resolved["max_mismatch"]),
        float(resolved["ssim"]),
        float(resolved["delta_e"]),
    )


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
            {
                "name": name,
                "tolerance": tolerance,
                "max_mismatch": max_mismatch,
                "pairs": [
                    {
                        "pair": pair,
                        "tolerance": pair_entry["tolerance"],
                        "max_mismatch": pair_entry["max_mismatch"],
                        "ssim": pair_entry["ssim"],
                        "delta_e": pair_entry["delta_e"],
                    }
                    for pair, pair_entry in sorted(
                        document["tests"][name]["pairs"].items()
                    )
                ],
            }
            for name, tolerance, max_mismatch in entries(document)
        ],
    }
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


def format_cpp_float(value: float) -> str:
    """A C++ double literal that round-trips to the same value (2.0 -> "2.0")."""

    text = repr(float(value))
    if text in ("inf", "-inf", "nan"):
        raise ToleranceError(f"cannot emit a non-finite value {text!r} into a C++ header")
    return text


def pairs_header(document: dict[str, Any], name: str) -> str:
    """The C++ table of per-pair tolerances for one test entry (issue #245)."""

    entry = document["tests"].get(name)
    if entry is None:
        raise ToleranceError(f"{name}: no tests entry to emit a pairs header from")
    pairs = entry["pairs"]
    rows = []
    for pair in sorted(pairs):
        resolved = pairs[pair]
        rows.append(
            "    {.pair = "
            f'"{pair}", '
            f'.edge_tolerance = {int(resolved["tolerance"])}, '
            f'.max_mismatch = {format_cpp_float(resolved["max_mismatch"])}, '
            f'.min_ssim = {format_cpp_float(resolved["ssim"])}, '
            f'.max_delta_e = {format_cpp_float(resolved["delta_e"])}}},'
        )
    body = "\n".join(rows)
    if body:
        body += "\n"
    return (
        "// SPDX-License-Identifier: MIT\n"
        "//\n"
        "// Generated by scripts/golden_tolerances.py from tests/golden/tolerance.json; do not edit.\n"
        f'// Per-backend-pair parity tolerances for the "{name}" test entry. A pair that is not\n'
        "// listed here resolves to the exact policy (0 / 0.0 / 1.0 / 0.0) in the parity harness.\n"
        "\n"
        "#pragma once\n"
        "\n"
        '#include "tessera/rendering/golden/parity.hpp"\n'
        "\n"
        "#include <array>\n"
        "\n"
        "namespace tessera::tests::gpu::backends {\n"
        "\n"
        "inline constexpr std::array<tessera::rendering::golden::ParityTolerance, "
        f"{len(pairs)}> kParityPairTolerances = {{{{\n"
        f"{body}"
        "}};\n"
        "\n"
        "} // namespace tessera::tests::gpu::backends\n"
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Read tests/golden/tolerance.json (issue #161, pairs from #245).",
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
        choices=("tolerance", "max_mismatch", "ssim", "delta_e"),
        help="with --name/--pair, print only that field's value",
    )
    parser.add_argument(
        "--pair",
        help="with --name, print only this backend pair (an unlisted pair is exact)",
    )
    parser.add_argument(
        "--pairs-header",
        type=Path,
        help="with --name, write the C++ per-pair tolerance table to this path",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.field is not None and args.name is None:
        parser.error("--field requires --name")
    if args.pair is not None and args.name is None:
        parser.error("--pair requires --name")
    if args.pairs_header is not None and args.name is None:
        parser.error("--pairs-header requires --name")
    if args.json and (args.name is not None or args.pair is not None):
        parser.error("--json cannot be combined with --name or --pair")
    if args.pairs_header is not None and args.field is not None:
        parser.error("--pairs-header cannot be combined with --field")
    if args.field in ("ssim", "delta_e") and args.pair is None:
        parser.error(f"--field {args.field} requires --pair")

    try:
        document = load(args.path)
    except ToleranceError as error:
        print(f"golden_tolerances: {error}", file=sys.stderr)
        return 1

    if args.json:
        sys.stdout.write(format_json(document))
        return 0

    if args.pairs_header is not None:
        try:
            header = pairs_header(document, args.name)
            args.pairs_header.write_text(header, encoding="utf-8")
        except (ToleranceError, OSError) as error:
            print(f"golden_tolerances: {error}", file=sys.stderr)
            return 1
        return 0

    if args.pair is not None:
        tolerance, max_mismatch, ssim, delta_e = resolve_pair(document, args.name, args.pair)
        if args.field == "tolerance":
            print(tolerance)
        elif args.field == "max_mismatch":
            print(format_number(max_mismatch))
        elif args.field == "ssim":
            print(format_number(ssim))
        elif args.field == "delta_e":
            print(format_number(delta_e))
        else:
            print(
                f"{args.name}\t{args.pair}\t{tolerance}\t{format_number(max_mismatch)}\t"
                f"{format_number(ssim)}\t{format_number(delta_e)}"
            )
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
