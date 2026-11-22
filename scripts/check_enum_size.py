#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Enum base-type gate (issue #734).

An `enum class` without an explicit underlying type is `int`: four bytes, whatever the value set
needs. The tree already knows this - 249 of its 366 `enum class` declarations name a base type, and
229 of those are `std::uint8_t` - but 117 did not, and 70 of those were in `include/`.

The cost is real but uneven, which is why this is a gate and not a sweep-and-forget. Patching every
`include/` declaration to `std::uint8_t` shrinks 21 tessera types and grows none, 192 bytes total;
seven of the shrinking types are on a per-frame or per-node path (`RuntimeTick` is the frame loop's
return value, `ui::text::TextStyle` is copied per text run, `PaintCache::Entry` is the paint cache's
per-node record). The rest do not move at all, because the enum already sat in tail padding - and
that is the point: the *next* field added to those structs lands in a byte that exists.

`.clang-tidy` already enables `performance-enum-size`, which is exactly this check, and it reports
79 warnings for 70 enums. Nothing failed, because clang-tidy is not run in CI. This script is the
cheap half of the fix named in the issue: it needs no build tree and no compiler.

A declaration is allowed to keep `int` when the value set needs it or when the type is an ABI/FFI
contract. Those are recorded in `scripts/enum-size-allowlist.txt` with a reason, so the exception is
reviewed rather than assumed.

Usage:
  python -B scripts/check_enum_size.py
  python -B scripts/check_enum_size.py --verbose    # list every typed declaration too
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
ALLOWLIST = SCRIPTS_DIR / "enum-size-allowlist.txt"

# The two trees the issue names. `third_party/` is vendored and not ours to restyle; `build*/` is
# not source; `tests/` and `tools/` are not shipped and are out of scope for the first landing.
SCAN_ROOTS = ("include", "src")
SOURCE_SUFFIXES = (".hpp", ".h", ".cpp", ".cc", ".cxx")

# `enum class Name {` or `enum class Name : type {`, at any indentation, possibly with the brace on
# the next line (the tree uses both). `enum struct` is the same thing and is matched too. A one-line
# body (`enum class E { A, B };`) is matched as well: the declaration is what matters here, not the
# enumerators, so the pattern stops at the opening brace and does not require the line to end there.
ENUM_RE = re.compile(
    r"^\s*enum\s+(?:class|struct)\s+(?P<name>[A-Za-z_]\w*)\s*(?P<base>:\s*[^/{]+?)?\s*(?P<brace>\{)?"
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


def read_allowlist(path: Path) -> Dict[str, str]:
    """`path:Name  reason` per line. The reason is required: an unexplained exception is a bug."""
    entries: Dict[str, str] = {}
    if not path.is_file():
        return entries
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split(None, 1)
        if len(parts) != 2 or not parts[1].strip():
            raise SystemExit("error: malformed allowlist line (need `path:Name  reason`): " + stripped)
        entries[parts[0]] = parts[1].strip()
    return entries


def scan(path: Path) -> List[Tuple[int, str, Optional[str]]]:
    """Every `enum class` in `path` as (line, name, base). `base` is None when untyped."""
    found: List[Tuple[int, str, Optional[str]]] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for number, line in enumerate(text.splitlines(), start=1):
        match = ENUM_RE.match(line)
        if match is None:
            continue
        base = match.group("base")
        found.append((number, match.group("name"), base.strip(": \t") if base else None))
    return found


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="also list every declaration that already names a base type",
    )
    args = parser.parse_args(argv)

    allowlist = read_allowlist(ALLOWLIST)
    untyped: List[str] = []
    typed = 0
    seen_allowlisted: set = set()

    for path in source_files():
        rel = path.relative_to(REPO_ROOT).as_posix()
        for line, name, base in scan(path):
            if base is not None:
                typed += 1
                if args.verbose:
                    print("note: {}:{}: {} : {}".format(rel, line, name, base))
                continue
            key = "{}:{}".format(rel, name)
            if key in allowlist:
                seen_allowlisted.add(key)
                continue
            untyped.append("{}:{}: enum class {} has no base type".format(rel, line, name))

    errors: List[str] = []
    for message in untyped:
        errors.append(
            message + "; add `: std::uint8_t` (or the width the value set needs), or record it in "
            "scripts/enum-size-allowlist.txt with a reason"
        )

    # An allowlist entry that no longer matches anything is rot: the declaration was fixed or
    # renamed, and the exception should go.
    for key in sorted(set(allowlist) - seen_allowlisted):
        errors.append(
            "{}: allowlist entry matches no untyped enum class; remove the line".format(key)
        )

    for message in errors:
        print("ERROR " + message)
    if errors:
        print(
            "enum-size gate: {} error(s), {} typed declaration(s) checked".format(
                len(errors), typed
            ),
            file=sys.stderr,
        )
        return 1
    print(
        "enum-size gate: 0 error(s), {} typed declaration(s), {} allowlisted".format(
            typed, len(seen_allowlisted)
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
