#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""One-shot helper for issue #734: add `#include <cstdint>` where the sweep introduced `std::uint8_t`.

Adding a base type to an `enum class` makes the header depend on `<cstdint>`, which many of them did
not include because they never named a fixed-width type before. MSVC's standard library pulls
`<cstdint>` in transitively through almost everything, so the Windows build stays green; libstdc++
does not, so GCC fails with "elaborated-type-specifier for a scoped enum must not use the 'class'
keyword". This is the same MSVC-vs-GCC class of defect the build-lanes skill lists.

The include goes in the file's own include block, in sorted position among the `<...>` includes, so
the result matches what clang-format and the include-order convention expect.

Not a gate and not shipped. `scripts/check_enum_size.py` is the gate.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

SCAN_ROOTS = ("include", "src")
SOURCE_SUFFIXES = (".hpp", ".h")

STD_INCLUDE_RE = re.compile(r"^#include\s*<(?P<name>[^>]+)>\s*$")


def source_files() -> List[Path]:
    found: List[Path] = []
    for root in SCAN_ROOTS:
        base = REPO_ROOT / root
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                found.append(path)
    return sorted(found)


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
        # A header with only quoted includes (or none): put it after the last include, which is
        # where a standard header belongs, or after `#pragma once` when there is no include at all.
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
    args = parser.parse_args(argv)

    changed = 0
    skipped: List[str] = []
    for path in source_files():
        rel = path.relative_to(REPO_ROOT).as_posix()
        text = path.read_text(encoding="utf-8")
        if not needs_cstdint(text):
            continue
        lines = text.splitlines(keepends=True)
        if insert_include(lines) is None:
            skipped.append(rel + " (no <...> include block to place it in)")
            continue
        changed += 1
        print(rel)
        if args.apply:
            path.write_text("".join(lines), encoding="utf-8", newline="")

    for message in skipped:
        print("SKIP " + message, file=sys.stderr)
    print(
        "cstdint sweep: {} header(s) {}{}".format(
            changed, "rewritten" if args.apply else "would change",
            ", {} skipped".format(len(skipped)) if skipped else "",
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
