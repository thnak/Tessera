#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Memory budgets with a two-sided ratchet (issue #13).

Issue #13 exists because memory regressions surface late and on mobile first: the same Qt Quick app
measures about 60 MB on Windows and about 200 MB on Android from PSS counting graphics memory,
DPR-squared bitmap scaling, swapchain and depth buffers, ICU data, font mmaps and full-size decodes,
and **none** of that shows up in a frame-time budget. This CLI is the memory budget and its ratchet;
the policy lives in `memory_budget_ratchet.py` and the numbers in `memory_budget_measure.py`, so each
can be read and tested apart.

The budget data is `tests/golden/perf/memory_budget.json`, beside the other perf goldens.

## Three kinds of row, three verdicts

| Row | Kind | Status |
|---|---|---|
| `sizeof_node`, `sizeof_input_event`, `sizeof_paint_data`, `sizeof_computed_style`, `sizeof_node_id` | ABI constant | **enforce** (0%/0%) |
| `plain_node_allocations`, `aria_table_allocations`, `paint_slot_bytes`, ... | counted in one process | **enforce** (0%/0%) |
| `resident_bytes_per_node`, `node_dirty_scratch_bytes` | OS resident memory | **warn** (generous band) |
| `android_pss_bytes`, `gpu_vma_category_bytes` | not measurable here | **unmeasured** (no number) |

**Why the sizes and the counts are enforced.** They are the same number on every run of the same
binary: `sizeof` is decided by the ABI, and an allocation *count* comes from a replaced global
`operator new` in one process. The measurement module proves this rather than assuming it -- it runs
the probe twice and fails the row if the two values differ -- so the 0%/0% band is a real invariant
and a failure means the type or the allocation behavior actually changed. These are also the rows a
regression arrives through: a node that grows by 16 bytes costs 160 KB on a 10k-node screen and shows
up nowhere else.

**Why RSS only warns.** It is an OS reading of a heap. It moves with page granularity, allocator
behavior, ASLR and whatever else the machine is doing, and the same binary on the same box varies by
megabytes -- measured across five runs here, the per-node figure ranged from 321 to 364 bytes between
the two lanes with identical `sizeof(Node)` of 312. A gate that fails at random gets deleted, and
deleting it is worse than the trend line this row provides, so RSS is measured and reported on every
run inside a generous band and never blocks. The generous bound that *does* block RSS lives in the
CTest tests (`memory_steady_state`, `generated_ui_memory`), where it is a leak guard with an
order-of-magnitude allowance, not a ratchet on a specific number.

**Why the unimplemented rows are declared and empty.** Android PSS needs an Android sample and an
emulator job that this repository does not have (V1 targets Windows and Linux); the GPU category
split needs a Vulkan allocator while every deterministic measurement runs on the null backend, which
reports zero allocator bytes by design. Both are recorded with a `reason` and **no budget**, and the
validator rejects a budget on an `unmeasured` entry. A plausible-looking number for a quantity
nobody measured reads as evidence and ratchets a fiction, so the file says so where a reader will see
it, and the gate prints the same gap on every run.

## Usage

    python -B scripts/check_memory_budget.py --check
    python -B scripts/check_memory_budget.py --check --sizes-only    # no RSS, no quiet machine
    python -B scripts/check_memory_budget.py --update                # re-pin; refuses to loosen
    python -B scripts/check_memory_budget.py --build                 # build the probe, then check

Exit codes: 0 no error, 1 a regression, a stale budget, a malformed budget file or a refused
`--update`, 2 usage error or a missing input (an unbuilt probe is not a pass).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

# Sibling stdlib-only imports, following the pattern the repository already uses for them (see
# check_preview_budget.py and gen_license_bundle.py): `scripts/` is not on sys.path when this file is
# run as `python -B scripts/<name>.py`, so it is added before the import.
sys.path.insert(0, str(SCRIPTS_DIR))
import memory_budget_measure as measure  # noqa: E402  (sibling script, stdlib only)
import memory_budget_ratchet as ratchet  # noqa: E402  (sibling script, stdlib only)

BUDGET = REPO_ROOT / "tests" / "golden" / "perf" / "memory_budget.json"


def relative(path: Path) -> str:
    """A report path that is stable across machines (never a temp directory or a drive letter)."""
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.name


def read_budget(path: Path) -> Dict:
    measure.require_file(path, "budget file")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ratchet.UsageError("{} is not valid JSON: {}".format(relative(path), exc)) from exc
    return ratchet.validate(document)


def report(
    budget_path: Path,
    measurements: Dict[str, float],
    errors: Sequence[str],
    warnings: Sequence[str],
    notes: Sequence[str],
    unchecked: Sequence[str],
    checked: int,
    as_json: bool,
) -> None:
    if as_json:
        print(
            json.dumps(
                {
                    "ok": not errors,
                    "gate": "memory-budget",
                    "measurements": {name: measurements[name] for name in sorted(measurements)},
                    "errors": list(errors),
                    "warnings": list(warnings),
                    "entries_checked": checked,
                    "unmeasured": list(unchecked),
                },
                indent=2,
                sort_keys=True,
            )
        )
        return
    print("memory budget: {}".format(relative(budget_path)))
    print("\n".join(ratchet.format_rows(measurements)))
    for message in notes:
        print("note: " + message)
    for message in warnings:
        print("WARN " + message)
    for message in errors:
        print("ERROR " + message)


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="compare the measurements with the budget")
    mode.add_argument(
        "--update",
        action="store_true",
        help="re-pin the budget from the measurements (refuses to loosen an entry)",
    )
    parser.add_argument(
        "--budget", default=str(BUDGET), help="budget file to read or write"
    )
    parser.add_argument(
        "--build-dir",
        default=None,
        help="build directory holding the memory probe (default: the first configured build/ preset)",
    )
    parser.add_argument(
        "--sizes-only",
        action="store_true",
        help="measure only the deterministic rows (no resident-memory sampling)",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="build the memory probe target before measuring (needs cmake on PATH)",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="this lane has no build tree: validate the budget file only, and report that the "
        "numbers are measured by the build lanes (used by scripts/gates.py)",
    )
    parser.add_argument("--json", action="store_true", dest="as_json", help="machine-readable report")
    return parser.parse_args(argv)


def build_probe(build_dir: Path) -> Optional[int]:
    """Builds the probe target in place. Returns an exit code to stop with, or None to continue."""
    preset = build_dir.name
    command = ["cmake", "--build", str(build_dir), "--target", measure.PROBE_TARGET]
    print("building {} in {} ...".format(measure.PROBE_TARGET, relative(build_dir)))
    try:
        result = subprocess.run(command, cwd=str(REPO_ROOT), check=False)
    except OSError as exc:
        print("SETUP ERROR: cannot run cmake: {}".format(exc), file=sys.stderr)
        return 2
    if result.returncode != 0:
        print(
            "SETUP ERROR: building {} failed (exit {}); configure --preset {} first".format(
                measure.PROBE_TARGET, result.returncode, preset
            ),
            file=sys.stderr,
        )
        return 2
    return None


def run_update(
    budget_path: Path,
    document: Dict,
    measurements: Dict[str, float],
    checked: Dict[str, Dict],
    sizes_only: bool,
    build_config: str,
) -> int:
    refused = ratchet.refused_by_update(checked, measurements)
    if refused:
        for message in refused:
            print("error: {}".format(message), file=sys.stderr)
        print(
            "error: --update refuses to loosen a budget; a raise is a reviewed edit of the budget "
            "file with the reason in the diff, because a re-pin on a loaded box records machine load "
            "as headroom",
            file=sys.stderr,
        )
        return 1
    updated = ratchet.repinned(
        document, measurements, measure.measurement_counts(sizes_only), build_config
    )
    try:
        budget_path.write_text(json.dumps(updated, indent=2) + "\n", encoding="utf-8")
    except OSError as exc:
        print("SETUP ERROR: cannot write {}: {}".format(relative(budget_path), exc), file=sys.stderr)
        return 2
    for message in ratchet.update_notes(checked, measurements):
        print("note: " + message)
    print(
        "memory budget written: {} entr(y/ies) re-pinned in {}".format(
            len(checked), relative(budget_path)
        )
    )
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    budget_path = Path(args.budget)

    try:
        document = read_budget(budget_path)
    except ratchet.UsageError as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return 2
    entries: Dict[str, Dict] = document["entries"]

    if args.no_build:
        # The aggregator (`scripts/gates.py`) runs in a job that is a checkout, a clang-format
        # install and this script -- no compiler, so no build tree and no probe. That job can still
        # check the half that needs no numbers: the file's shape, and that every row is one of the
        # three statuses with a budget where a budget belongs. It reports the measurement it did not
        # do rather than pretending the budgets passed.
        measured, unmeasured = ratchet.classified(entries)
        enforced = sum(1 for entry in measured.values() if entry["status"] == "enforce")
        print("memory budget: {}".format(relative(budget_path)))
        print(
            "note: --no-build: the budget file is valid ({} enforce, {} warn, {} unmeasured); the "
            "numbers are measured by the build lanes (CTest `memory_budget` and "
            "`python -B scripts/check_memory_budget.py --check --sizes-only --build-dir build/<preset>`), "
            "which is where a regression fails".format(
                enforced, len(measured) - enforced, len(unmeasured)
            )
        )
        return 0

    build_dir = Path(args.build_dir) if args.build_dir else measure.default_build_dir()
    if build_dir is None:
        print(
            "SETUP ERROR: no configured build directory found under build/; configure a preset or "
            "pass --build-dir.",
            file=sys.stderr,
        )
        return 2
    if args.build:
        if build_probe(build_dir) is not None:
            return 2

    try:
        measurements, data_errors, data_warnings, build_config = measure.measure(
            build_dir, sizes_only=args.sizes_only
        )
    except measure.SetupError as exc:
        print("SETUP ERROR: {}".format(exc), file=sys.stderr)
        return 2

    # A budget recorded from one build configuration is not a claim about another: a checked-STL
    # Debug build has a different sizeof(Node) (320 vs 312 on MSVC), a larger PaintData and one extra
    # proxy allocation per vector. Rather than compare two different builds and report a flaky gate,
    # this refuses and says which configuration the file was recorded from.
    recorded_config = document.get("measurement", {}).get("build_config")
    if not build_config:
        errors = [
            "the memory probe did not report its build configuration; the budget is recorded from "
            "{!r} and a number from an unknown build is not comparable to it".format(
                recorded_config or "no configuration"
            )
        ]
        report(budget_path, measurements, errors, [], [], [], 0, args.as_json)
        print("memory-budget gate: 1 error(s) (build configuration)", file=sys.stderr)
        return 1
    if recorded_config and build_config != recorded_config:
        errors = [
            "this probe was built as {} but the budget was recorded from {}; a Debug and a Release "
            "build are different claims (different sizeof and allocation counts), so re-record with "
            "`--update` on a {} build or check the {} build".format(
                build_config, recorded_config, recorded_config, recorded_config
            )
        ]
        report(budget_path, measurements, errors, [], [], [], 0, args.as_json)
        print("memory-budget gate: 1 error(s) (build configuration)", file=sys.stderr)
        return 1

    # A sizes-only run skips the sampled rows, so their entries are out of scope rather than
    # unmeasured. The `unmeasured` status is reserved for a row no lane can measure.
    skipped = list(measure.SAMPLED_ROWS) if args.sizes_only else []
    # The GPU rows are out of scope on a lane that built no GPU probe or has no device (issue #703).
    # That is a lane capability, not a regression: the Windows CI lane configures with
    # TESSERA_BUILD_VULKAN=OFF, so it has no `gpu_memory_probe` and no device to measure. Reporting
    # those entries as missing would fail every such lane for a number it cannot produce, which is
    # exactly the "a gate that fails for an unrelated reason trains people to ignore it" failure the
    # source-size gate documents.
    if not measure.gpu_rows_available(build_dir):
        skipped.extend(measure.GPU_DETERMINISTIC_ROWS)
    checked = ratchet.in_scope(entries, skipped)
    checked = ratchet.in_scope(
        checked, [name for name, entry in entries.items() if entry["status"] == "unmeasured"]
    )

    if args.update:
        if data_errors:
            for message in data_errors:
                print("error: {}".format(message), file=sys.stderr)
            print(
                "error: --update wrote nothing: the measurements are not reproducible, so re-pinning "
                "them would record a measurement bug as the budget",
                file=sys.stderr,
            )
            return 1
        return run_update(
            budget_path, document, measurements, checked, args.sizes_only, build_config
        )

    errors, warnings, notes = ratchet.compare(checked, measurements, document["tolerance"])
    # Measurement-data problems (a "deterministic" row that moved) are errors whatever the entry's
    # status: they invalidate the number the gate is about to trust.
    errors = list(data_errors) + errors
    warnings = list(data_warnings) + warnings
    unmeasured = ratchet.unmeasured_notes(entries, measurements)
    report(
        budget_path,
        measurements,
        errors,
        warnings,
        list(notes) + unmeasured,
        [name for name, entry in sorted(entries.items()) if entry["status"] == "unmeasured"],
        len(checked),
        args.as_json,
    )

    if errors:
        print(
            "memory-budget gate: {} error(s), {} warning(s), {} entr(y/ies) checked".format(
                len(errors), len(warnings), len(checked)
            ),
            file=sys.stderr,
        )
        return 1
    if not args.as_json:
        print(
            "memory-budget gate: 0 error(s), {} warning(s), {} entr(y/ies) checked".format(
                len(warnings), len(checked)
            )
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
