#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Preview size and latency budgets with a two-sided ratchet (issue #170, stage P6; #395 the rest).

Stage P6 of `docs/design/reference-equantic-ui.md` section 7.4 and the consequences of
[ADR-045](../../decisions/ADR-045-browser-hosted-studio.md): the preview page is reached by a URL, so
its download size and its time to first frame are product concerns. This CLI is their measurement and
their ratchet; the policy lives in `preview_budget_ratchet.py` and the numbers in
`preview_budget_measure.py`, so each can be read and tested apart.

## The budgets

| Entry | What it measures | Status |
|---|---|---|
| `download_gzip_bytes` | gzip-9 bytes of every module the studio loads, its loaders, and the page's HTML/CSS/JS | **enforce** |
| `cold_engine_ready_ms` | instantiate a module plus the first compile, calibration-normalized, minimum of 5 fresh processes | **enforce** |
| `edit_compile_ms` | calibration-normalized per-edit compile over 30 edits x 5 fresh processes, after warm-up | **enforce** |

Each module's own compressed size is also reported on every run (`tessera_canvas.wasm.gzip` and
friends) but is not gated: one entry on the total fails once rather than N times for one change, and
the breakdown names the file that grew.

**Why the size row is enforced.** It is fully deterministic: the same files and the same zlib build
give the same count on every machine and every run, so its 5%/1% band is measurement slack rather than
noise cover.

**What the latency rows measure, and why that is now enforceable (issue #395).** They used to warn,
because a wall-clock number on a shared runner moves with the runner: the eight Pages runs of
2026-11-16/17 saw `cold_engine_ready_ms` between 16.552 and 28.272 ms and `edit_compile_ms` between
0.083 and 0.223 ms with no relevant source change. A band tight enough to catch a real regression
would have failed at random. The row is therefore no longer raw wall clock. Each probe process times
a fixed integer workload, and the row is `measured_ms * reference_ms / calibration_ms`: the reference
box's speed cancels, so the number means "how long this takes on the recorded reference profile"
rather than "how fast the machine that ran this happened to be".

Measured on one box, 15 fresh processes: raw spread 1.06x (1.08x with twelve CPU-burner processes on
twelve threads), normalized spread 1.04x. The calibration absorbs both CPU-speed and contention,
because the loop and the compile share a core, a governor and a process.

**What that does not prove, stated so a reviewer can disagree.** It assumes the ratio between integer
arithmetic speed and Wasm-compile speed is roughly constant across x86-64 runner generations. Only
one box was measured. The `worse` allowance on the latency rows is set generously for exactly that
reason, and `*_raw_ms` is printed on every run so a human can tell "the code got slower" from "the
runner class changed". If a future lane shows the ratio is not stable across hosts, the honest
response is to demote these rows back to `warn` and ratchet a work counter instead -- the ratchet
code is unchanged either way, so that is a `status` edit plus a note in the budget file, not a
redesign.

**Why every module is weighed.** ADR-054 decision 3 ships three lazy `import()`s and the studio loads
all three: diagnostics for the default view, scene preview for the Scene tab, canvas for the Canvas
tab. Measuring diagnostics alone left the largest module (canvas, ~1.29 MB gzip against
diagnostics' ~0.38 MB) outside the budget entirely.

Tolerances are **per side** (`{"better": %, "worse": %}`). A single symmetric band cannot express both
claims: wide enough for a noisy timing row means 300%, and `budget * (1 - 3)` is negative, so no entry
could ever be stale and half the ratchet would be dead. `better` must stay under 100%, and the budget
file's validator rejects anything else.

**The latency rows are the engine's share only.** Cold time-to-first-frame also needs the browser's
HTML/CSS parse and a WebGPU frame (P3, not landed); edit-to-view needs that frame too. Rather than put
a made-up number behind a name that promises the whole path, the entries are named for what was
measured, and the browser lane adds `cold_first_frame_ms`/`edit_to_view_ms` when there is a frame to
time. The budget file's `note` says the same thing where a reader of the data will see it.

## Usage

    python -B scripts/check_preview_budget.py --check
    python -B scripts/check_preview_budget.py --update
    python -B scripts/check_preview_budget.py --check --size-only     # no Node, size rows only
    python -B scripts/check_preview_budget.py --check --no-build      # no module: validate only

Exit codes: 0 no error, 1 a regression, a stale budget, a malformed budget file or a refused
`--update`, 2 usage error or a missing input (an unbuilt module is not a pass).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

# Sibling stdlib-only imports, following the pattern the repository already uses for them (see
# check_pins.py and gen_license_bundle.py): `scripts/` is not on sys.path when this file is run as
# `python -B scripts/<name>.py`, so it is added before the import.
sys.path.insert(0, str(SCRIPTS_DIR))
import preview_budget_measure as measure  # noqa: E402  (sibling script, stdlib only)
import preview_budget_ratchet as ratchet  # noqa: E402  (sibling script, stdlib only)

BUDGET = REPO_ROOT / "tests" / "golden" / "perf" / "preview_budget.json"


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
    checked: int,
    as_json: bool,
) -> None:
    if as_json:
        print(
            json.dumps(
                {
                    "ok": not errors,
                    "gate": "preview-budget",
                    "measurements": {name: measurements[name] for name in sorted(measurements)},
                    "errors": list(errors),
                    "warnings": list(warnings),
                    "entries_checked": checked,
                },
                indent=2,
                sort_keys=True,
            )
        )
        return
    print("preview budget: {}".format(relative(budget_path)))
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
        help="re-pin the budget from the measurements (refuses to loosen an enforced entry)",
    )
    parser.add_argument(
        "--module-dir",
        default=None,
        help="directory holding every module the studio loads (tessera_diagnostics, "
        "tessera_scene_preview and tessera_canvas, .js and .wasm; default: build/pages, then "
        "build/wasm-webgpu/src/compiler/codegen)",
    )
    parser.add_argument(
        "--node",
        default=measure.default_node(),
        help="Node executable for the latency probe (default: $EMSDK_NODE, else node)",
    )
    parser.add_argument("--budget", default=str(BUDGET), help="budget file to read or write")
    parser.add_argument("--json", action="store_true", dest="as_json", help="machine-readable report")
    parser.add_argument(
        "--size-only",
        action="store_true",
        help="measure the size rows only (no Node, no latency rows)",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="this lane has no built modules: validate the budget file only, and report that the "
        "numbers are measured by the Pages lane (used by scripts/gates.py)",
    )
    return parser.parse_args(argv)


def run_update(
    budget_path: Path,
    document: Dict,
    measurements: Dict[str, float],
    checked: Dict[str, Dict],
    reference_ms: float,
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
        document, measurements, measure.encoder(), measure.measurement_counts(reference_ms)
    )
    try:
        budget_path.write_text(json.dumps(updated, indent=2) + "\n", encoding="utf-8")
    except OSError as exc:
        print("SETUP ERROR: cannot write {}: {}".format(relative(budget_path), exc), file=sys.stderr)
        return 2
    for message in ratchet.update_notes(checked, measurements):
        print("note: " + message)
    print(
        "preview budget written: {} entr(y/ies) re-pinned in {}".format(
            len(checked), relative(budget_path)
        )
    )
    return 0


def reference_calibration(document: Dict, required: bool) -> float:
    """The calibration the latency pins were recorded against, or a `UsageError`.

    It lives in the budget file rather than in code because it is part of the claim: every latency
    pin is `measured_ms * reference_ms / calibration_ms`, so changing `reference_ms` rebases every
    latency number. That is the same reason `encoder` is stored for the size rows -- a silent change
    must be impossible, and a deliberate one must show up in a diff.
    """
    measurement = document.get("measurement")
    value = measurement.get("calibration_reference_ms") if isinstance(measurement, dict) else None
    if value is None:
        if not required:
            return 0.0
        raise ratchet.UsageError(
            "the budget file has no `measurement.calibration_reference_ms`; the latency rows are "
            "calibration-normalized, so the reference they were pinned against must be recorded"
        )
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        raise ratchet.UsageError(
            "`measurement.calibration_reference_ms` is {!r}; it must be a positive number of "
            "milliseconds".format(value)
        )
    return float(value)


def run_no_build(budget_path: Path, document: Dict) -> int:
    """`--no-build`: validate the file and report what this lane could not measure.

    The aggregator (`scripts/gates.py`) runs in a job that is a checkout and a few lint tools: no
    Emscripten, no assembled site, no Node. That job can still check the half that needs no numbers
    -- the file's shape, its tolerances and its calibration contract -- and must say plainly that the
    numbers were not measured here rather than print a pass that reads like the budgets passed.
    """
    entries: Dict[str, Dict] = document["entries"]
    enforced = sorted(name for name, entry in entries.items() if entry["status"] == "enforce")
    warned = sorted(name for name, entry in entries.items() if entry["status"] == "warn")
    print("preview budget: {}".format(relative(budget_path)))
    print(
        "note: --no-build: the budget file is valid ({} enforce, {} warn); the numbers are measured "
        "by the Pages lane (`.github/workflows/pages.yml`, `--module-dir build/pages --node "
        "$EMSDK_NODE`), which is where a regression fails".format(len(enforced), len(warned))
    )
    print("note: enforced here: {}".format(", ".join(enforced) if enforced else "(none)"))
    if warned:
        print("note: warn-only here: {}".format(", ".join(warned)))
    # Validate the calibration contract too, even though this lane cannot measure: a file that names
    # a latency pin without the reference it was recorded against is malformed, and this is the one
    # lane that runs on every PR -- the cheapest place to catch it. `required=False` so a file whose
    # only measured rows are sizes stays valid.
    reference_calibration(document, required=False)
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
        try:
            return run_no_build(budget_path, document)
        except ratchet.UsageError as exc:
            print("error: {}".format(exc), file=sys.stderr)
            return 2

    # Whether a latency row is in scope decides whether the calibration contract is required: a
    # size-only run never divides by it, so demanding it there would fail a lane that only weighs bytes.
    checks_latency = not args.size_only
    try:
        reference_ms = reference_calibration(document, required=checks_latency)
    except ratchet.UsageError as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return 2

    module_dir = Path(args.module_dir) if args.module_dir else measure.default_module_dir()
    if module_dir is None:
        print(
            "SETUP ERROR: no Wasm module found. Build `tessera_diagnostics_wasm`, "
            "`tessera_scene_preview_wasm` and `tessera_canvas_wasm` or pass --module-dir; checked "
            "{}.".format(", ".join(relative(candidate) for candidate in measure.MODULE_CANDIDATES)),
            file=sys.stderr,
        )
        return 2

    try:
        measurements = measure.measure(
            module_dir, args.node, reference_ms, size_only=args.size_only
        )
    except measure.SetupError as exc:
        print("SETUP ERROR: {}".format(exc), file=sys.stderr)
        return 2

    # A size-only run skips the latency rows, so their entries are out of scope rather than unmeasured.
    checked = ratchet.in_scope(
        entries, measure.LATENCY_ENTRIES if args.size_only else ()
    )

    if args.update:
        return run_update(budget_path, document, measurements, checked, reference_ms)

    errors, warnings, notes = ratchet.compare(checked, measurements, document["tolerance"])
    # The per-file breakdown is checked on both a full and a size-only run: it is a size fact, and a
    # size-only lane is exactly the cheap run that can hold it to the measurement.
    errors = errors + ratchet.breakdown_drift(checked, measurements)
    report(budget_path, measurements, errors, warnings, notes, len(checked), args.as_json)

    if errors:
        print(
            "preview-budget gate: {} error(s), {} warning(s), {} entr(y/ies) checked".format(
                len(errors), len(warnings), len(checked)
            ),
            file=sys.stderr,
        )
        return 1
    if not args.as_json:
        print(
            "preview-budget gate: 0 error(s), {} warning(s), {} entr(y/ies) checked".format(
                len(warnings), len(checked)
            )
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
