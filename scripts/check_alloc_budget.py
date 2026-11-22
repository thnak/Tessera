#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Allocations-per-frame budget with a one-way ratchet (issue #701 step 4).

`docs/specs/memory-model-v1.md` section 2 says "no `malloc`/`new` in steady-state frame code after
warm-up", and section 9 asks for allocations/frame as a metric. Step 1 of #701 built the measurement
(`frame_alloc_probe`); this is the gate that keeps the numbers from drifting back.

The budget data is `tests/golden/perf/alloc_budget.json`, beside the other perf goldens. The
measurement is `tests/benchmarks/memory/alloc_per_frame_tests.cpp`, which replaces the global
`operator new` and counts every call in the process, so a row is exact for a given binary rather
than sampled.

## Why this is a separate gate from `check_memory_budget.py`

Different subject and different failure mode. The memory budget pins `sizeof` and one-shot allocation
counts -- ABI constants that cannot move without a code change. This pins *per-frame* behaviour,
which is the number a regression actually arrives through: a helper that allocates once per node per
frame is invisible in a `sizeof` row and shows up here as a 601x jump.

## Why the budget is per lane

The counts are **not** the same on MSVC and GCC, and the difference is real rather than noise: the
standard library's containers and the two compilers' inlining decide how many separate blocks a
frame's work becomes, and MSVC's checked iterators and its `std::string` small-buffer size differ
from libstdc++'s. A single shared table would either fail on one lane or be so loose on both that it
caught nothing. So the file holds one table per lane, the gate selects the one it is running on, and
it refuses to compare across lanes -- the same rule `check_memory_budget.py` applies to build
configurations.

## The ratchet

* a row that **exceeds** its budget -> ERROR (a regression),
* a row **below** its budget -> WARN (tighten it with `--update`),
* a row in the file that the test did not report -> ERROR (the budget must not rot),
* a row the test reported that the file does not name -> ERROR (an unrecorded measurement).

`--update` lowers budgets and adds new rows; it refuses to raise one, so it can only record a
decision, never hide a regression.

## Determinism

The test is run twice and the two runs must agree exactly, the same rule `check_memory_budget.py`
applies to its counted rows. A row that wobbles between runs is reported as an error rather than
compared, because a gate that fails at random gets deleted.

Output is deterministic and sorted. Exit codes: 0 = no errors, 1 = at least one error, 2 = usage
error.

Usage:
  python -B scripts/check_alloc_budget.py --check --build-dir build/release
  python -B scripts/check_alloc_budget.py --update --build-dir build/release
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
BUDGET = REPO_ROOT / "tests" / "golden" / "perf" / "alloc_budget.json"

# The test prints one line per row: `<scene> <action> <actual> (budget <n>)`.
ROW = re.compile(r"^(?P<scene>[a-z-]+)\s+(?P<action>[a-z-]+)\s+(?P<actual>\d+)\s+\(budget\s+\d+\)$")


def current_lane() -> str:
    """The budget table this run belongs to: `msvc` on Windows, `gcc` everywhere else.

    The two lanes measure different numbers for the same code (see the module docstring), so the
    table is selected rather than shared. A lane with no table is a setup error, not a pass: a gate
    that silently compares against nothing is worse than one that fails.
    """
    return "msvc" if sys.platform == "win32" else "gcc"


def find_test(build_dir: Path) -> Optional[Path]:
    """The alloc_per_frame_tests binary under `build_dir`, or None."""
    for candidate in build_dir.rglob("alloc_per_frame_tests*"):
        if candidate.is_file() and candidate.suffix in ("", ".exe"):
            return candidate
    return None


def run_once(binary: Path) -> Tuple[Optional[Dict[str, int]], str]:
    """One run's rows, or (None, why) when the run failed."""
    try:
        completed = subprocess.run(
            [str(binary)], capture_output=True, text=True, encoding="utf-8", timeout=600
        )
    except (OSError, subprocess.TimeoutExpired) as failure:
        return None, str(failure)
    rows: Dict[str, int] = {}
    for line in completed.stdout.splitlines():
        match = ROW.match(line.strip())
        if match is not None:
            rows["{}/{}".format(match.group("scene"), match.group("action"))] = int(
                match.group("actual")
            )
    if not rows:
        return None, "no measurement rows in the output (exit {})".format(completed.returncode)
    return rows, ""


def read_budget(path: Path) -> dict:
    if not path.is_file():
        raise SystemExit("error: budget file not found: {}".format(path))
    return json.loads(path.read_text(encoding="utf-8"))


def write_budget(data: dict, path: Path) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="compare against the budget (default)")
    parser.add_argument(
        "--update",
        action="store_true",
        help="lower budgets and add new rows; never raises one",
    )
    parser.add_argument(
        "--build-dir",
        default="build/release",
        help="the build tree holding alloc_per_frame_tests (default: build/release)",
    )
    args = parser.parse_args(argv)

    build_dir = (REPO_ROOT / args.build_dir).resolve()
    binary = find_test(build_dir)
    if binary is None:
        print(
            "SETUP ERROR: alloc_per_frame_tests not found under {}; build the target first".format(
                build_dir
            ),
            file=sys.stderr,
        )
        return 1

    first, why = run_once(binary)
    if first is None:
        print("SETUP ERROR: the first run produced no rows: {}".format(why), file=sys.stderr)
        return 1
    second, why = run_once(binary)
    if second is None:
        print("SETUP ERROR: the second run produced no rows: {}".format(why), file=sys.stderr)
        return 1

    data = read_budget(BUDGET)
    lane = current_lane()
    lanes: dict = data.get("lanes", {})
    if lane not in lanes:
        print(
            "SETUP ERROR: the budget has no table for lane {!r}; record one with --update".format(lane),
            file=sys.stderr,
        )
        return 1
    rows: dict = lanes[lane].get("rows", {})

    errors: List[str] = []
    warnings: List[str] = []

    # Determinism first: a row that moves between two runs of the same binary is not a number this
    # gate can compare, and reporting it as a regression would be a lie.
    for key in sorted(set(first) | set(second)):
        if first.get(key) != second.get(key):
            errors.append(
                "{}: measured {} then {}; a counted row must be identical across runs".format(
                    key, first.get(key), second.get(key)
                )
            )

    if args.update:
        raised = [
            (key, rows[key]["budget"], first[key])
            for key in first
            if key in rows and first[key] > rows[key]["budget"]
        ]
        if raised:
            for key, old, new in sorted(raised):
                print(
                    "error: {} grew from {} to {} allocations/frame; fix it instead of raising "
                    "the budget".format(key, old, new),
                    file=sys.stderr,
                )
            return 1
        for key in sorted(first):
            if key in rows:
                if first[key] < rows[key]["budget"]:
                    print(
                        "note: {}: tightened from {} to {} allocations/frame".format(
                            key, rows[key]["budget"], first[key]
                        )
                    )
                rows[key]["budget"] = first[key]
            else:
                rows[key] = {
                    "budget": first[key],
                    "measures": "recorded by --update; describe what this row measures",
                }
                print("note: {}: added at {} allocations/frame".format(key, first[key]))
        data["lanes"] = lanes
        write_budget(data, BUDGET)
        print("alloc budget written: {} row(s) for lane {}".format(len(rows), lane))
        return 0

    for key in sorted(first):
        if key not in rows:
            errors.append(
                "{}: measured {} allocations/frame but the budget does not name this row; "
                "record it with --update".format(key, first[key])
            )
            continue
        budget = rows[key]["budget"]
        if first[key] > budget:
            errors.append(
                "{}: {} allocations/frame exceeds the {} budget".format(key, first[key], budget)
            )
        elif first[key] < budget:
            warnings.append(
                "{}: {} allocations/frame is below the {} budget; tighten it with --update".format(
                    key, first[key], budget
                )
            )

    for key in sorted(rows):
        if key not in first:
            errors.append(
                "{}: the budget names this row but the test did not report it; remove the "
                "entry".format(key)
            )

    for message in warnings:
        print("WARN " + message)
    for message in errors:
        print("ERROR " + message)

    print(
        "alloc-budget gate: {} error(s), {} warning(s), {} row(s) checked on lane {}".format(
            len(errors), len(warnings), len(first), lane
        )
    )
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
