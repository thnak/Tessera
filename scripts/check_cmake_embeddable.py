#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embeddable-build gate: the framework's CMake must not read the top-level project's directories.

`tessera new` (route B) builds an app by `add_subdirectory(<checkout>)` from the *app's* project. In
that layout `CMAKE_SOURCE_DIR` and `CMAKE_BINARY_DIR` name the app, not Tessera, so every
`${CMAKE_SOURCE_DIR}/tools/codegen/main.cpp` in the framework points into the app and the configure
step fails. About 300 uses had drifted in because nothing built the framework that way; the framework
tree used `PROJECT_SOURCE_DIR` for its own paths in most places and `CMAKE_SOURCE_DIR` in the rest.

The rule: framework CMake uses `PROJECT_SOURCE_DIR` / `PROJECT_BINARY_DIR` (Tessera's own root, set by
the `project(Tessera)` call, whoever the parent is), `CMAKE_CURRENT_*` or `CMAKE_CURRENT_LIST_DIR`.

The one legitimate use is the standalone-build test `CMAKE_SOURCE_DIR STREQUAL
CMAKE_CURRENT_SOURCE_DIR` ("is this directory the top level?"), which compares the two and so cannot
point into a parent. It is the only form this gate accepts.

Needs no build tree and no compiler. Scans CMakeLists.txt and *.cmake outside third_party/ and build
output.

Usage:
  python -B scripts/check_cmake_embeddable.py
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

# Vendored code keeps its own conventions; build output and test fixtures are not the framework.
SKIP_DIRS = ("third_party", "node_modules", "out")
SKIP_PREFIXES = ("build", "scripts/tests/fixtures")

BANNED_RE = re.compile(r"\bCMAKE_(SOURCE|BINARY)_DIR\b")
# `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`: the "am I top level" idiom.
TOP_LEVEL_RE = re.compile(
    r"\bCMAKE_SOURCE_DIR\s+STREQUAL\s+CMAKE_CURRENT_SOURCE_DIR\b"
    r"|\bCMAKE_CURRENT_SOURCE_DIR\s+STREQUAL\s+CMAKE_SOURCE_DIR\b"
)


def cmake_files() -> List[Path]:
    found: List[Path] = []
    for path in REPO_ROOT.rglob("*"):
        if not path.is_file():
            continue
        if path.name != "CMakeLists.txt" and path.suffix != ".cmake":
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        parts = rel.split("/")
        # Dot-directories hold tool state and agent worktrees (full copies of the tree), never source.
        if any(part in SKIP_DIRS or part.startswith(".") for part in parts[:-1]):
            continue
        if any(rel.startswith(prefix) for prefix in SKIP_PREFIXES):
            continue
        found.append(path)
    return sorted(found)


def violations(path: Path) -> List[str]:
    found: List[str] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for number, line in enumerate(text.splitlines(), start=1):
        code = line.split("#", 1)[0]
        if not BANNED_RE.search(code):
            continue
        if TOP_LEVEL_RE.search(code) and len(BANNED_RE.findall(code)) == 1:
            continue
        found.append("{}:{}: {}".format(path.relative_to(REPO_ROOT).as_posix(), number, line.strip()))
    return found


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.parse_args(argv)
    bad: List[str] = []
    files = cmake_files()
    for path in files:
        bad.extend(violations(path))
    if bad:
        print("error: framework CMake reads the top-level project's directory (breaks `tessera new`):")
        for entry in bad:
            print("  " + entry)
        print("use PROJECT_SOURCE_DIR / PROJECT_BINARY_DIR (Tessera's own root) instead")
        return 1
    print("cmake-embeddable: {} CMake files, none read CMAKE_SOURCE_DIR / CMAKE_BINARY_DIR".format(len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
