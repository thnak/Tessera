#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Preview size and latency budgets with a two-sided ratchet (issue #170, stage P6).

Stage P6 of `docs/design/reference-equantic-ui.md` section 7.4 and the consequences of
[ADR-045](../../decisions/ADR-045-browser-hosted-studio.md): the preview page is reached by a URL, so
its download size and its time to first frame are product concerns. This CLI is their measurement and
their ratchet; the policy lives in `preview_budget_ratchet.py` and the numbers in
`preview_budget_measure.py`, so each can be read and tested apart.

## The three budgets

| Entry | What it measures | Status |
|---|---|---|
| `download_gzip_bytes` | gzip-9 bytes of the module + its loader + the page's HTML/CSS/JS: the transfer a browser pays | **enforce** |
| `cold_engine_ready_ms` | instantiate the module plus the first compile, minimum of 5 fresh processes | **warn** |
| `edit_compile_ms` | minimum per-edit compile over 30 edits x 5 fresh processes, after warm-up | **warn** |

**Why the size row is enforced.** It is fully deterministic: the same files and the same zlib build
give the same count on every machine and every run, so its 5%/1% band is measurement slack rather than
noise cover. It is also the row the slice's acceptance is actually about ("compressed download"),
and shrinking the module is a product win the ratchet should protect.

**Why the latency rows only warn.** Wall clock on a shared runner is not reproducible to a useful
tolerance; this repository says so in `docs/guides/testing.md` section 7 and enforced it in the
counter-only design of `tests/benchmarks/stylesheet`. The mitigations -- a 300% worse-side allowance
and the minimum over fresh processes (contention only ever *adds* time, so the smallest sample is the
closest to the true cost) -- make the warning a real order-of-magnitude detector, but "a gate that
fails at random is worse than no gate". Two entries, two verdicts, and the reason is the noise, not
the importance: cold first frame and edit-to-view matter more than the download size, which is exactly
why they get measured and reported on every run instead of being trusted to an unreliable gate.

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
        help="directory holding tessera_diagnostics.js and .wasm (default: build/pages, then "
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
    return parser.parse_args(argv)


def run_update(
    budget_path: Path,
    document: Dict,
    measurements: Dict[str, float],
    checked: Dict[str, Dict],
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
        document, measurements, measure.encoder(), measure.measurement_counts()
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


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    budget_path = Path(args.budget)

    try:
        document = read_budget(budget_path)
    except ratchet.UsageError as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return 2
    entries: Dict[str, Dict] = document["entries"]

    module_dir = Path(args.module_dir) if args.module_dir else measure.default_module_dir()
    if module_dir is None:
        print(
            "SETUP ERROR: no Wasm module found. Build `tessera_diagnostics_wasm` or pass "
            "--module-dir; checked {}.".format(
                ", ".join(relative(candidate) for candidate in measure.MODULE_CANDIDATES)
            ),
            file=sys.stderr,
        )
        return 2

    try:
        measurements = measure.measure(module_dir, args.node, size_only=args.size_only)
    except measure.SetupError as exc:
        print("SETUP ERROR: {}".format(exc), file=sys.stderr)
        return 2

    # A size-only run skips the latency rows, so their entries are out of scope rather than unmeasured.
    checked = ratchet.in_scope(
        entries, measure.LATENCY_ENTRIES if args.size_only else ()
    )

    if args.update:
        return run_update(budget_path, document, measurements, checked)

    errors, warnings, notes = ratchet.compare(checked, measurements, document["tolerance"])
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
