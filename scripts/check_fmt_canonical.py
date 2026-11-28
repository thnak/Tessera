#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""`tessera_fmt --check` over the shipped Tessera sources (issue #990, ADR-061 section 5).

Scope: every .tsx, .css and tessera.json under samples/ and packages/tessera-ui/. Those files
predate the formatter and goldens read their line numbers, so reformatting them wholesale is its
own decision. Until it is made this is a ratchet:

  * a file listed in tools/fmt/fmt-baseline.txt is known not canonical; it must still exist and
    still be non-canonical (once it is formatted, delete its line, so the list only shrinks);
  * every other file in scope must be canonical, so new samples are born formatted.

Usage: check_fmt_canonical.py --fmt <tessera_fmt> [--root <repo>] [--baseline <file>]
Exit 0 when the tree matches, 1 otherwise.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Callable, Iterable

SCOPES = ("samples", "packages/tessera-ui")
SUFFIXES = (".tsx", ".css")
SKIP_DIRS = {"node_modules", "build", ".git"}


def in_scope(root: Path) -> list[str]:
    found: list[str] = []
    for scope in SCOPES:
        base = root / scope
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or SKIP_DIRS & set(path.relative_to(root).parts):
                continue
            if path.suffix in SUFFIXES or path.name == "tessera.json":
                found.append(path.relative_to(root).as_posix())
    return sorted(found)


def read_baseline(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    lines = (line.strip() for line in path.read_text(encoding="utf-8").splitlines())
    return {line for line in lines if line and not line.startswith("#")}


def check(files: Iterable[str], baseline: set[str], is_canonical: Callable[[str], bool]) -> list[str]:
    problems: list[str] = []
    present = set(files)
    for name in sorted(present):
        canonical = is_canonical(name)
        if name in baseline and canonical:
            problems.append(f"{name}: now canonical, remove it from the baseline")
        elif name not in baseline and not canonical:
            problems.append(f"{name}: not canonical (run tessera_fmt on it)")
    for name in sorted(baseline - present):
        problems.append(f"{name}: in the baseline but not in the tree, remove it")
    return problems


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fmt", required=True, help="path to the tessera_fmt binary")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--baseline", default=None)
    args = parser.parse_args(argv)
    root = Path(args.root)
    fmt = str(Path(args.fmt).resolve())
    baseline = Path(args.baseline) if args.baseline else root / "tools" / "fmt" / "fmt-baseline.txt"

    def is_canonical(name: str) -> bool:
        result = subprocess.run([fmt, "--check", name], cwd=root, capture_output=True, text=True)
        if result.returncode not in (0, 1):
            print(f"{name}: tessera_fmt failed ({result.returncode}): {result.stdout.strip()}", file=sys.stderr)
            return False
        return result.returncode == 0

    files = in_scope(root)
    problems = check(files, read_baseline(baseline), is_canonical)
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"fmt check: {len(files)} file(s), {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
