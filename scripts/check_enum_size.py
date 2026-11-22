#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Enum base-type gate (issue #734).

An `enum class` with no explicit underlying type is `int`: 4 bytes, whatever the value set needs.
The tree already knows this -- 205 of its 275 `include/` declarations name a base type, and most of
those are `std::uint8_t` -- but 116 declarations did not, and `.clang-tidy` enables
`performance-enum-size`, which reports exactly this. Nothing failed, because clang-tidy is not run
in CI (no workflow mentions `lint` or `tidy`, and `scripts/gates.py` has no lint gate), so the check
was configured and ignored.

This gate is the cheap half of that: it needs no build tree, no compile database and no clang-tidy
run, so it can sit in `gates.py` next to the other source checks.

It is a **ratchet**, like `check_source_size.py`. `scripts/enum-size-baseline.txt` records every
untyped declaration that existed when the gate landed, with the reason it is allowed to stay:

* an untyped declaration that is **not** in the baseline -> ERROR (a new violation),
* a baseline entry whose declaration is now **typed** -> WARN (stale: remove the line),
* a baseline entry naming a file that no longer exists -> ERROR (the baseline must not rot).

So the only way to touch a baselined declaration is to type it, and `--update` drops the entries
that no longer apply (it never adds one).

**Why a baseline at all, rather than failing on all 116?** The same reason the source-size gate has
one: turning `main` red for a pre-existing condition forces a 116-declaration sweep into whatever
unrelated change happens to run the gate first. The sweep is issue #734's own work; the gate's job
is to stop the number growing while that lands.

Excluded, with the reason recorded in the script:

* `third_party/` -- vendored, not ours to restyle,
* `build*/`, `.git/` -- not source,
* `tests/` -- a test may declare a local enum to exercise a template; it is not a shipped type.

The file list comes from `git ls-files` for the same reason `check_source_size.py` does (issue #733
item 6): a directory walk sees agent worktrees that are not part of the change under review.

Output is deterministic and sorted: ``ERROR path:line name`` / ``WARN path:line name``. Exit codes:
0 = no errors (warnings are fine), 1 = at least one error, 2 = usage error.

Usage:
  python -B scripts/check_enum_size.py
  python -B scripts/check_enum_size.py --update    # drop entries that are now typed
  python -B scripts/check_enum_size.py --list      # print every untyped declaration
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
BASELINE = SCRIPTS_DIR / "enum-size-baseline.txt"

SOURCE_SUFFIXES = (".hpp", ".h", ".cc", ".cxx", ".cpp")

# Directories whose files are exempt, with the reason the exemption exists.
EXEMPT_PARTS: Dict[str, str] = {
    "third_party": "vendored dependency, not ours to restyle",
    "tests": "a test-local enum is not a shipped type",
}
EXEMPT_PREFIXES = ("build", ".git")

# `enum class Name {` / `enum struct Name {`, capturing an optional `: base`.
DECL = re.compile(r"\benum\s+(?:class|struct)\s+(\w+)\s*(?::\s*([\w:]+))?\s*\{")


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


def tracked_files() -> Optional[List[str]]:
    """Every path git tracks, or None outside a git checkout (see check_source_size.py)."""
    if shutil.which("git") is None:
        return None
    try:
        completed = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "ls-files", "-z"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return [name for name in completed.stdout.decode("utf-8", "replace").split("\0") if name]


def source_files() -> List[str]:
    tracked = tracked_files()
    if tracked is not None:
        return sorted(
            rel for rel in tracked if Path(rel).suffix in SOURCE_SUFFIXES and not is_exempt(rel)
        )
    found = []
    for path in REPO_ROOT.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        if is_exempt(rel):
            continue
        found.append(rel)
    return sorted(found)


def strip_comments(text: str) -> str:
    """Blank out comments, keeping newlines so line numbers stay right."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def untyped_declarations(rel: str) -> List[Tuple[int, str]]:
    """(line, name) for every `enum class` in `rel` with no explicit base type."""
    try:
        text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    clean = strip_comments(text)
    found = []
    for match in DECL.finditer(clean):
        if match.group(2) is not None:
            continue
        found.append((clean[: match.start()].count("\n") + 1, match.group(1)))
    return found


def scan() -> Dict[str, List[Tuple[int, str]]]:
    """Every untyped declaration, grouped by file, in a deterministic order."""
    result: Dict[str, List[Tuple[int, str]]] = {}
    for rel in source_files():
        found = untyped_declarations(rel)
        if found:
            result[rel] = found
    return result


def read_baseline(path: Path) -> Dict[str, int]:
    """`path name count` per line: the number of untyped declarations allowed in that file."""
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
        "# Untyped `enum class` declarations (no explicit underlying type, so `int`) that",
        "# existed when the enum-size gate landed (issue #734), with the count per file. The",
        "# gate fails on a new one or on any growth past these numbers, so the only edit this",
        "# file accepts is a lower count (scripts/check_enum_size.py --update). Delete a line",
        "# once the file has no untyped declaration left.",
        "#",
        "# path count",
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
        "--list",
        action="store_true",
        help="print every untyped declaration, not just the violations",
    )
    args = parser.parse_args(argv)

    found = scan()
    counts = {rel: len(items) for rel, items in found.items()}
    baseline = read_baseline(BASELINE)

    if args.update:
        raised = [
            (rel, baseline[rel], n) for rel, n in counts.items() if rel in baseline and n > baseline[rel]
        ]
        if raised:
            for rel, old, new in sorted(raised):
                print(
                    "error: {} grew from {} to {} untyped declarations; type them instead of "
                    "raising the baseline".format(rel, old, new),
                    file=sys.stderr,
                )
            return 1
        write_baseline(counts, BASELINE)
        print(
            "enum-size baseline written: {} untyped declaration(s) in {} file(s)".format(
                sum(counts.values()), len(counts)
            )
        )
        return 0

    errors: List[str] = []
    warnings: List[str] = []

    for rel, items in sorted(found.items()):
        pinned = baseline.get(rel)
        if pinned is None:
            for line, name in items:
                errors.append(
                    "{}:{} {}: untyped `enum class` (implicitly `int`); name a base type, "
                    "usually `: std::uint8_t`".format(rel, line, name)
                )
        elif len(items) > pinned:
            for line, name in items[pinned:]:
                errors.append(
                    "{}:{} {}: untyped `enum class` exceeds this file's {} declaration "
                    "baseline; type it instead of adding another".format(rel, line, name, pinned)
                )

    for rel in sorted(baseline):
        if rel not in counts:
            if not (REPO_ROOT / rel).is_file():
                errors.append(
                    "{}: baseline entry names a file that does not exist; remove the line".format(rel)
                )
            else:
                warnings.append(
                    "{}: baseline entry is stale (no untyped declarations left); remove the "
                    "line".format(rel)
                )
        elif counts[rel] <= baseline[rel]:
            # Grandfathered: still visible, so the sweep has a worklist, but not a failure.
            warnings.append(
                "{}: {} untyped declaration(s) (grandfathered at {}; type them when next "
                "edited)".format(rel, counts[rel], baseline[rel])
            )

    if args.list:
        for rel, items in sorted(found.items()):
            for line, name in items:
                print("note: {}:{} {}".format(rel, line, name))

    for message in warnings:
        print("WARN " + message)
    for message in errors:
        print("ERROR " + message)

    total = sum(counts.values())
    print(
        "enum-size gate: {} error(s), {} warning(s), {} untyped declaration(s) in {} file(s), "
        "{} file(s) checked".format(
            len(errors), len(warnings), total, len(counts), len(source_files())
        )
    )
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
