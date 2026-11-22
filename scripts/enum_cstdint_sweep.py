#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""One-shot helper for issue #734: add `#include <cstdint>` to the files the enum sweep touched.

Adding a base type to an `enum class` makes the file depend on `<cstdint>`, which many of them did
not include because they never named a fixed-width type before. MSVC's standard library pulls
`<cstdint>` in transitively through almost everything, so the Windows build stays green; libstdc++
does not, so GCC fails with "elaborated-type-specifier for a scoped enum must not use the 'class'
keyword". This is the same MSVC-vs-GCC class of defect the build-lanes skill lists.

Scoped deliberately: it only touches files that differ from the given revision, so it cannot sweep
the whole repository. The include goes in sorted position among the `<...>` includes, or after the
last include when the file has none.

Not a gate and not shipped. `scripts/check_enum_size.py` is the gate.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

STD_INCLUDE_RE = re.compile(r"^#include\s*<(?P<name>[^>]+)>\s*$")


def changed_files(rev: str) -> List[Path]:
    """Files that differ from `rev`, so this cannot touch anything the sweep did not."""
    result = subprocess.run(
        ["git", "diff", "--name-only", rev],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    paths = []
    for line in result.stdout.splitlines():
        rel = line.strip()
        if not rel:
            continue
        path = REPO_ROOT / rel
        if path.is_file() and path.suffix in (".hpp", ".h", ".cpp", ".cc", ".cxx"):
            paths.append(path)
    return sorted(paths)


def needs_cstdint(text: str) -> bool:
    return bool(re.search(r"std::uint(?:8|16|32|64)_t", text)) and "#include <cstdint>" not in text


def insert_include(lines: List[str]) -> Optional[int]:
    """Insert `#include <cstdint>` in sorted position; return the index used, or None."""
    std_indices = []
    any_include = []
    for index, line in enumerate(lines):
        match = STD_INCLUDE_RE.match(line.rstrip("\r\n"))
        if match is not None:
            std_indices.append((index, match.group("name")))
        if line.lstrip().startswith("#include"):
            any_include.append(index)
    if not std_indices:
        if any_include:
            lines.insert(any_include[-1] + 1, "#include <cstdint>\n")
            return any_include[-1] + 1
        for index, line in enumerate(lines):
            if line.lstrip().startswith("#pragma once"):
                lines.insert(index + 1, "\n#include <cstdint>\n")
                return index + 1
        return None
    for index, name in std_indices:
        if name > "cstdint":
            lines.insert(index, "#include <cstdint>\n")
            return index
    last = std_indices[-1][0]
    lines.insert(last + 1, "#include <cstdint>\n")
    return last + 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--apply", action="store_true", help="write the changes")
    parser.add_argument(
        "--since",
        default="HEAD",
        help="only consider files that differ from this revision (default: HEAD)",
    )
    args = parser.parse_args(argv)

    changed = 0
    skipped: List[str] = []
    for path in changed_files(args.since):
        rel = path.relative_to(REPO_ROOT).as_posix()
        text = path.read_text(encoding="utf-8")
        if not needs_cstdint(text):
            continue
        lines = text.splitlines(keepends=True)
        if insert_include(lines) is None:
            skipped.append(rel + " (no include block to place it in)")
            continue
        changed += 1
        print(rel)
        if args.apply:
            # newline="" keeps the file's own line endings; no BOM, which a PowerShell
            # `Set-Content -Encoding UTF8` would add and which breaks the SPDX first line.
            path.write_text("".join(lines), encoding="utf-8", newline="")

    for message in skipped:
        print("SKIP " + message, file=sys.stderr)
    print(
        "cstdint sweep: {} file(s) {}{}".format(
            changed, "rewritten" if args.apply else "would change",
            ", {} skipped".format(len(skipped)) if skipped else "",
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
