#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""WGSL validation gate (ADR-049 WebGPU promotion condition 2).

Runs the pinned naga CLI on every authored ``*.wgsl`` under ``src/`` (the shaders the WebGPU
backend embeds and ships). naga parses and validates each module and exits non-zero on any error.
The naga version must match ``tools/naga-version.txt``; a different version is an error because
validation rules move between releases.

Exit codes: 0 every file validated, 1 a file failed or the naga version differs, 2 setup failure
(naga not found, no WGSL files).

Usage:
  python -B scripts/ci/validate_wgsl.py [--naga PATH] [--repo-root PATH] [--allow-version-mismatch]

Install naga with ``cargo install naga-cli --version "$(cat tools/naga-version.txt)" --locked``.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[2]


def find_naga(explicit: Optional[str]) -> Optional[str]:
    if explicit:
        return explicit if Path(explicit).is_file() else None
    found = shutil.which("naga")
    if found:
        return found
    for base in (os.environ.get("CARGO_HOME"), str(Path.home() / ".cargo")):
        if not base:
            continue
        for name in ("naga", "naga.exe"):
            candidate = Path(base) / "bin" / name
            if candidate.is_file():
                return str(candidate)
    return None


def wgsl_files(repo_root: Path) -> List[Path]:
    return sorted((repo_root / "src").rglob("*.wgsl"))


def naga_version(naga: str) -> str:
    result = subprocess.run([naga, "--version"], capture_output=True, text=True, check=False)
    return result.stdout.strip()


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--naga", default=None, help="path to the naga executable")
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument(
        "--allow-version-mismatch",
        action="store_true",
        help="warn instead of failing when naga differs from tools/naga-version.txt",
    )
    args = parser.parse_args(argv)
    repo_root = Path(args.repo_root).resolve()

    naga = find_naga(args.naga)
    if naga is None:
        print("SETUP ERROR: naga not found (cargo install naga-cli --locked)", file=sys.stderr)
        return 2

    pin_file = repo_root / "tools" / "naga-version.txt"
    pinned = pin_file.read_text(encoding="utf-8").strip() if pin_file.is_file() else ""
    actual = naga_version(naga)
    version_ok = bool(pinned) and actual == pinned
    if not version_ok:
        message = "naga {} found, pinned {}".format(actual or "?", pinned or "(missing pin)")
        if not args.allow_version_mismatch:
            print("FAIL: " + message, file=sys.stderr)
            return 1
        print("WARNING: " + message, file=sys.stderr)

    files = wgsl_files(repo_root)
    if not files:
        print("SETUP ERROR: no *.wgsl files under {}".format(repo_root / "src"), file=sys.stderr)
        return 2

    failures = 0
    for path in files:
        relative = path.relative_to(repo_root).as_posix()
        result = subprocess.run(
            [naga, str(path)], capture_output=True, text=True, encoding="utf-8", errors="replace"
        )
        if result.returncode == 0:
            print("PASS {}".format(relative))
        else:
            failures += 1
            print("FAIL {}".format(relative))
            print((result.stdout or "") + (result.stderr or ""), end="")
    print("{} WGSL file(s), {} failed (naga {})".format(len(files), failures, actual))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
