#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""One-shot helper for issue #734: add an explicit base type to every untyped `enum class`.

Not a gate and not shipped - it is the mechanical half of the sweep, kept in the tree so the change
is reproducible and reviewable rather than a 65-file hand edit. `scripts/check_enum_size.py` is the
gate that keeps it from coming back.

The width is chosen from the enumerator count, not guessed:

  <= 256 enumerators  -> std::uint8_t
  <= 65536            -> std::uint16_t
  otherwise           -> std::uint32_t

An enum whose body cannot be read (a macro, an `#if` inside the body) is reported and left alone for
a human. `--apply` writes; without it the script only prints what it would do.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

SCAN_ROOTS = ("include", "src")
SOURCE_SUFFIXES = (".hpp", ".h", ".cpp", ".cc", ".cxx")

ENUM_RE = re.compile(
    r"^(?P<indent>\s*)enum\s+(?:class|struct)\s+(?P<name>[A-Za-z_]\w*)\s*(?P<base>:\s*[^/{]+?)?\s*(?P<brace>\{)?"
)


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


def enumerator_count(lines: List[str], start: int, has_brace: bool) -> Optional[int]:
    """Count top-level enumerators of the enum whose declaration is at `start` (0-based).

    Returns None when the body cannot be read confidently: a preprocessor directive inside it, or a
    brace that never closes.
    """
    index = start
    if not has_brace:
        index += 1
        if index >= len(lines) or "{" not in lines[index]:
            return None
    depth = 0
    count = 0
    seen_token = False
    while index < len(lines):
        line = lines[index]
        if line.lstrip().startswith("#"):
            return None
        for char in line:
            if char == "{":
                depth += 1
                if depth == 1:
                    continue
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return count if seen_token else 0
            elif depth == 1:
                if char == ",":
                    count += 1
                    seen_token = False
                elif not char.isspace():
                    seen_token = True
        index += 1
    return None


def width_for(count: int) -> str:
    if count <= 256:
        return "std::uint8_t"
    if count <= 65536:
        return "std::uint16_t"
    return "std::uint32_t"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--apply", action="store_true", help="write the changes")
    args = parser.parse_args(argv)

    changed = 0
    skipped: List[str] = []
    for path in source_files():
        rel = path.relative_to(REPO_ROOT).as_posix()
        text = path.read_text(encoding="utf-8")
        lines = text.splitlines(keepends=True)
        edits: List[Tuple[int, str]] = []
        for number, line in enumerate(lines):
            match = ENUM_RE.match(line.rstrip("\r\n"))
            if match is None or match.group("base") is not None:
                continue
            count = enumerator_count(lines, number, match.group("brace") is not None)
            if count is None:
                skipped.append("{}:{}: {} (body not readable)".format(rel, number + 1, match.group("name")))
                continue
            base = width_for(count)
            # Insert `: base` before the brace, keeping the line's own trailing newline.
            body = line.rstrip("\r\n")
            newline = line[len(body):]
            if match.group("brace") is not None:
                new_body = body.replace("{", ": {} {{".format(base), 1)
            else:
                new_body = body + ": " + base
            edits.append((number, new_body + newline))
            changed += 1
        if edits and args.apply:
            for number, new_line in edits:
                lines[number] = new_line
            path.write_text("".join(lines), encoding="utf-8", newline="")
        for number, new_line in edits:
            print("{}:{}: {}".format(rel, number + 1, new_line.strip()))

    for message in skipped:
        print("SKIP " + message, file=sys.stderr)
    print(
        "enum-base sweep: {} declaration(s) {}{}".format(
            changed, "rewritten" if args.apply else "would change",
            ", {} skipped".format(len(skipped)) if skipped else "",
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
