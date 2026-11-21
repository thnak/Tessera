#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""C++ source-size gate (AGENTS.md, Code conventions).

`AGENTS.md` says a source file has a *soft* limit of 300 lines and a *hard* limit of 500, with no
exceptions but the Phase 0 PoC targets. Nothing enforced it, so files drifted over unnoticed until
someone measured by hand (issue #327). This gate is the enforcement.

It is a **ratchet**, not a cliff. When the gate was added, 25 files were already over the hard
limit; failing on all of them would have turned `main` red and forced a 25-file refactor into an
unrelated change. Instead `scripts/source-size-baseline.txt` records each file that was over the
limit at the moment the gate landed, with its then-current line count:

* an over-limit file that is **not** in the baseline  -> ERROR (a new violation),
* an over-limit file whose count **grew** past its baseline -> ERROR (the violation got worse),
* an over-limit file at or **below** its baseline -> WARN (grandfathered, still visible),
* a baseline entry that is now **under** the limit -> WARN (stale: remove the line),
* a baseline entry naming a file that no longer exists -> ERROR (the baseline must not rot).

So the only way to touch those files is to make them smaller, and `--update` lowers a baseline to
the current counts (it never raises one).

Excluded from the limit, with the reason recorded in the script:

* `tools/poc/`, `tests/golden/` -- Phase 0 PoC targets and generated goldens (`.g.hpp` is codegen
  output; nobody hand-splits it), both named as exceptions in the definition of done,
* `third_party/` -- vendored, not ours to restyle,
* `build*/`, `.git/` -- not source.

Output is deterministic and sorted: ``ERROR path`` / ``WARN path``. Exit codes:
0 = no errors (warnings are fine), 1 = at least one error, 2 = usage error.

Usage:
  python -B scripts/check_source_size.py
  python -B scripts/check_source_size.py --update      # lower the ratchet after a real split
  python -B scripts/check_source_size.py --verbose     # list every over-soft-limit file too
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
BASELINE = SCRIPTS_DIR / "source-size-baseline.txt"

HARD_LIMIT = 500
SOFT_LIMIT = 300

SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".cc", ".cxx")

# Directories whose files are exempt from the limit, with the reason the exemption exists. A path
# matches when any of its parts names one of these; `build`/`build-*`/`build_*` trees are covered by
# the prefix test below because their names vary per lane.
EXEMPT_PARTS: Dict[str, str] = {
    "third_party": "vendored dependency, not ours to restyle",
    "poc": "Phase 0 proof-of-concept target (definition of done exception)",
    "golden": "generated golden (codegen output, never hand-split)",
    # The Android app shell's Gradle/NDK build tree (issue #189). Gradle writes it under
    # android/app/.cxx, which is not a `build*` prefix, so the prefix test below does not catch it.
    ".cxx": "Android NDK build tree, not source",
    ".gradle": "Gradle cache, not source",
}
EXEMPT_PREFIXES = ("build", ".git")


def is_exempt(rel: str) -> Optional[str]:
    if rel.split("/")[0].startswith(EXEMPT_PREFIXES):
        return "build tree"
    for part in rel.split("/")[:-1]:
        if part.startswith(EXEMPT_PREFIXES):
            return "build tree"
        reason = EXEMPT_PARTS.get(part)
        if reason:
            return reason
    return None


def source_files() -> List[str]:
    found: List[str] = []
    for path in REPO_ROOT.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        if is_exempt(rel):
            continue
        found.append(rel)
    return sorted(found)


def line_count(rel: str) -> int:
    # Count like an editor does: a trailing newline does not add a line, and CRLF counts once.
    text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
    if not text:
        return 0
    return text.count("\n") + (0 if text.endswith("\n") else 1)


def read_baseline(path: Path) -> Dict[str, int]:
    entries: Dict[str, int] = {}
    if not path.is_file():
        return entries
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split()
        if len(parts) != 2 or not parts[1].isdigit():
            raise SystemExit("error: malformed baseline line: {}".format(stripped))
        entries[parts[0]] = int(parts[1])
    return entries


def write_baseline(entries: Dict[str, int], path: Path) -> None:
    lines = [
        "# SPDX-License-Identifier: MIT",
        "#",
        "# Files above the 500-line hard limit (AGENTS.md, Code conventions) when the",
        "# source-size gate landed, with the line count at that moment. The gate fails on a",
        "# new violation or on any growth past these numbers, so the only edit this file",
        "# accepts is a lower count (scripts/check_source_size.py --update). Delete a line",
        "# once the file is split under the limit.",
        "#",
        "# path lines",
    ]
    for rel in sorted(entries):
        lines.append("{} {}".format(rel, entries[rel]))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite the baseline from the current tree (lowers counts, never raises them)",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="also list files over the soft limit but under the hard limit",
    )
    args = parser.parse_args(argv)

    counts: Dict[str, int] = {rel: line_count(rel) for rel in source_files()}
    over: Dict[str, int] = {rel: n for rel, n in counts.items() if n > HARD_LIMIT}
    baseline = read_baseline(BASELINE)

    if args.update:
        raised = [
            (rel, baseline[rel], n)
            for rel, n in over.items()
            if rel in baseline and n > baseline[rel]
        ]
        if raised:
            for rel, old, new in sorted(raised):
                print(
                    "error: {} grew from {} to {} lines; split it instead of raising the "
                    "baseline".format(rel, old, new),
                    file=sys.stderr,
                )
            return 1
        write_baseline(over, BASELINE)
        print(
            "source-size baseline written: {} file(s) above the {} hard limit".format(
                len(over), HARD_LIMIT
            )
        )
        return 0

    errors: List[str] = []
    warnings: List[str] = []

    for rel, count in sorted(over.items()):
        pinned = baseline.get(rel)
        if pinned is None:
            errors.append(
                "{}: {} lines exceeds the {} hard limit; split it by responsibility".format(
                    rel, count, HARD_LIMIT
                )
            )
        elif count > pinned:
            errors.append(
                "{}: {} lines exceeds its {} line baseline and the {} hard limit; "
                "split it instead of growing it".format(rel, count, pinned, HARD_LIMIT)
            )
        else:
            warnings.append(
                "{}: {} lines exceeds the {} hard limit (grandfathered at {}; "
                "split when next edited)".format(rel, count, HARD_LIMIT, pinned)
            )

    for rel in sorted(baseline):
        if rel not in counts:
            errors.append(
                "{}: baseline entry names a file that does not exist; remove the line".format(rel)
            )
        elif counts[rel] <= HARD_LIMIT:
            warnings.append(
                "{}: baseline entry is stale ({} lines, under the {} limit); remove the "
                "line".format(rel, counts[rel], HARD_LIMIT)
            )

    if args.verbose:
        for rel, count in sorted(counts.items()):
            if SOFT_LIMIT < count <= HARD_LIMIT:
                print("note: {}: {} lines exceeds the {} soft limit".format(rel, count, SOFT_LIMIT))

    for message in warnings:
        print("WARN " + message)
    for message in errors:
        print("ERROR " + message)

    if errors:
        print(
            "source-size gate: {} error(s), {} warning(s), {} file(s) checked".format(
                len(errors), len(warnings), len(counts)
            ),
            file=sys.stderr,
        )
        return 1
    print(
        "source-size gate: 0 error(s), {} warning(s), {} file(s) checked".format(
            len(warnings), len(counts)
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
