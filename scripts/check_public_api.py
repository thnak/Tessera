#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Public-API surface gate (issue #76).

Nothing recorded Tessera's public surface, so a breaking change could land with nobody noticing: a
header deleted, a type renamed, a `::part()` name dropped. This gate is the record and the check.

`scripts/public-api-baseline.txt` lists the surface with one canonical line per fact, sorted, and
`--check` fails when the tree and the file disagree in either direction:

* a line the tree has and the baseline does not -> **ADDED**: the surface grew and the baseline was
  not updated (run `--update`, which records it);
* a line the baseline has and the tree does not -> **REMOVED**: this is the breaking change the gate
  exists for (a deleted header, a renamed type, a dropped prop or part).

`--update` is the ratchet, following `scripts/check_source_size.py`: it records additions, but it
**refuses to write anything at all when a line would be dropped or renamed**. A removal is therefore
a hand edit of the baseline file in a reviewed change, and the deleted line is the record of the
break with the reason in the diff. Loosening the surface can never happen by running a script.

## What it records

Declaration-level only (the module docstring of `scripts/public_api_surface.py` is authoritative):

* every `include/tessera/**/*.hpp` as a `header` line, plus the class/struct/union/enum, alias,
  concept, function, constant and variable declared at namespace scope in it;
* every `packages/tessera-ui/**/*.tsx` as a `component` line, its `export type <Name>Props` fields as
  `prop` lines, and its literal `part="a b"` names as `part` lines (the `::part()` surface of
  ADR-047 item 3);
* and nothing else. Not signatures, not class members, not default values, not prop types, not
  enumerators, not the `tessera.json` schema (issue #76 defers config schemas to #194).

That under-promise is deliberate: a gate that fires on every signature edit gets switched off, which
is worse than no gate. What it does catch is the surface *disappearing*, which is never routine.

## Determinism

Output is sorted by path, then kind, then name, with no timestamps, absolute paths or version
numbers, so two runs over one tree are byte-identical (asserted in
`scripts/tests/test_check_public_api.py`).

Usage:
  python -B scripts/check_public_api.py --check
  python -B scripts/check_public_api.py --update
  python -B scripts/check_public_api.py --check --repo-root PATH

Exit codes: 0 = the surface matches (or the baseline was written), 1 = a difference or a refused
`--update`, 2 = usage error or a missing input.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

# Sibling stdlib-only import, following the pattern the repository already uses (check_pins.py,
# gen_license_bundle.py): `scripts/` is not on sys.path when this file is run as
# `python -B scripts/<name>.py`, so it is added before the import.
sys.path.insert(0, str(SCRIPTS_DIR))
import public_api_surface as surface  # noqa: E402  (sibling script, stdlib only)

BASELINE = SCRIPTS_DIR / "public-api-baseline.txt"
BASELINE_RELATIVE = "scripts/public-api-baseline.txt"


def compare(baseline: Sequence[str], current: Sequence[str]) -> Tuple[List[str], List[str]]:
    """`(added, removed)`: lines only in `current`, then lines only in `baseline`. Both sorted.

    The two sides are the whole policy. A membership test, not a line-by-line walk, because the
    baseline is a set of facts and a rename is exactly one removal plus one addition.
    """
    present = set(current)
    recorded = set(baseline)
    added = sorted(present - recorded, key=surface.sort_key)
    removed = sorted(recorded - present, key=surface.sort_key)
    return added, removed


def describe(line: str) -> str:
    """`kind path name` as a readable fact (used by the JSON report's docs and by the tests)."""
    parts = line.split(" ", 2)
    kind = parts[0] if parts else ""
    path = parts[1] if len(parts) > 1 else ""
    name = parts[2] if len(parts) > 2 else ""
    if kind == surface.HEADER and not name:
        return "header {}".format(path)
    return "{} {} ({})".format(kind, name, path)


def diff_messages(added: Sequence[str], removed: Sequence[str]) -> List[str]:
    """One actionable message per line, added first then removed, both already sorted."""
    messages: List[str] = []
    for line in added:
        messages.append("ERROR {}: not in the baseline; run --update to record it".format(line))
    for line in removed:
        messages.append(
            "ERROR {}: recorded in the baseline but gone from the tree; if the removal is "
            "intentional delete the line from {} in this change (--update refuses to), otherwise "
            "restore it".format(line, BASELINE_RELATIVE)
        )
    return messages


def path_count(lines: Sequence[str]) -> int:
    """How many distinct files the surface lines mention."""
    return len({line.split(" ", 2)[1] for line in lines if " " in line})


def break_advice(added: Sequence[str], removed: Sequence[str]) -> None:
    """A closing hint, printed once, naming which half of the diff needs review."""
    if removed:
        print(
            "note: {} line(s) removed or renamed = BREAKING. Update the baseline by hand for those "
            "and say why in the change description; a minor version may break pre-1.0, but the "
            "break must be visible (docs/specs/versioning-v1.md section 1).".format(len(removed))
        )
    if added:
        print(
            "note: {} line(s) added = additive; `python -B scripts/check_public_api.py --update` "
            "records them.".format(len(added))
        )


def json_report(
    added: Sequence[str],
    removed: Sequence[str],
    current: Sequence[str],
    recorded: Sequence[str],
    baseline_is_missing: bool,
) -> str:
    """The `--json` document: one sorted list per side, plus the counts, so CI can annotate."""
    payload = {
        "schema": 1,
        "gate": "public-api",
        "ok": not added and not removed,
        "baseline": BASELINE_RELATIVE,
        "baseline_missing": baseline_is_missing,
        "current_lines": len(current),
        "baseline_lines": len(recorded),
        "added": list(added),
        "removed": list(removed),
    }
    return json.dumps(payload, indent=2, sort_keys=True)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Public-API surface gate (issue #76)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Exit codes: 0 the surface matches (or the baseline was written), 1 a difference or a\n"
            "refused --update, 2 a usage error or a missing input."
        ),
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="compare the tree against the baseline (the default when neither flag is given)",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="record additions in the baseline; refuses to drop or rename a line",
    )
    parser.add_argument(
        "--repo-root",
        default=str(REPO_ROOT),
        help="repository root to scan (default: the checkout with this script)",
    )
    parser.add_argument("--json", action="store_true", dest="as_json", help="machine-readable report")
    parser.add_argument("--quiet", action="store_true", help="print only the summary line")
    args = parser.parse_args(argv)

    repo_root = Path(args.repo_root).resolve()
    if not (repo_root / surface.CPP_ROOT).is_dir():
        print(
            "error: no {} under {}; --repo-root must name a Tessera checkout".format(
                surface.CPP_ROOT, repo_root
            ),
            file=sys.stderr,
        )
        return 2

    baseline_path = repo_root / "scripts" / BASELINE.name
    current, warnings = surface.collect_surface(repo_root)
    current_text = surface.render_baseline(current)
    baseline_text = (
        baseline_path.read_text(encoding="utf-8") if baseline_path.is_file() else ""
    )
    recorded = surface.read_baseline(baseline_text)

    for warning in warnings:
        print("WARN " + warning)

    if args.update:
        added, removed = compare(recorded, current)
        if removed:
            for line in removed:
                print(
                    "ERROR {}: present in the baseline and absent from the tree".format(line),
                    file=sys.stderr,
                )
            print(
                "error: refusing to update {}: {} line(s) would be dropped. A removed or renamed "
                "declaration is a breaking change, so it must be a reviewed edit of the file (the "
                "deleted line is the record), not a script run. Nothing was written.".format(
                    BASELINE_RELATIVE, len(removed)
                ),
                file=sys.stderr,
            )
            return 1
        if baseline_text == current_text:
            print(
                "public-api gate: baseline is already current ({} line(s), {} file(s))".format(
                    len(current), path_count(current)
                )
            )
            return 0
        baseline_path.write_text(current_text, encoding="utf-8", newline="\n")
        print(
            "public-api baseline written: {} line(s), {} file(s), +{} additive".format(
                len(current), path_count(current), len(added)
            )
        )
        return 0

    added, removed = compare(recorded, current)
    if args.as_json:
        print(
            json_report(added, removed, current, recorded, baseline_is_missing=not recorded)
        )
        return 0 if not added and not removed else 1

    for message in diff_messages(added, removed):
        print(message)

    if not added and not removed and baseline_text != current_text:
        # The lines all match, but the file's bytes do not: it is unsorted, has stray whitespace, or
        # was hand-edited. --update rewrites it without touching a single line of the surface.
        print(
            "ERROR {}: content is not in canonical form (unsorted, or hand-edited); run --update "
            "to rewrite it. It deleted no recorded line, so no break was lost.".format(
                BASELINE_RELATIVE
            )
        )

    if added or removed or baseline_text != current_text:
        print(
            "public-api gate: {} added, {} removed, {} line(s) in the tree, {} in the baseline"
            .format(len(added), len(removed), len(current), len(recorded)),
            file=sys.stderr,
        )
        if not args.quiet:
            break_advice(added, removed)
        return 1

    print(
        "public-api gate: 0 added, 0 removed, {} line(s) across {} file(s)".format(
            len(current), path_count(current)
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())