#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify that clang-format matches the pinned formatter version.

The single source of truth is ``tools/format-version.txt``: CI installs
``clang-format==<that version>`` and this gate fails when the binary on PATH (or ``$CLANG_FORMAT``)
reports a different major.minor.patch. Exit codes: 0 match, 1 mismatch, 2 setup failure (missing
version file, malformed pin, missing tool, unparseable output).

    python -B scripts/check_format_version.py
    python -B scripts/check_format_version.py --json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
VERSION_FILE = Path("tools") / "format-version.txt"
PINNED_RE = re.compile(r"^\d+\.\d+\.\d+$")
VERSION_RE = re.compile(r"clang-format version (\d+\.\d+\.\d+)")
FALLBACK_VERSION_RE = re.compile(r"version (\d+\.\d+\.\d+)")
INSTALL_HINT = "python -m pip install --user clang-format=={}"


class SetupError(Exception):
    """Missing/malformed pin, missing tool or unparseable tool output (exit code 2)."""


def read_expected(repo_root: Path) -> str:
    path = repo_root / VERSION_FILE
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise SetupError("cannot read the pinned formatter version from {}: {}".format(path, error))
    for line in lines:
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        if not PINNED_RE.match(text):
            raise SetupError(
                "{} must contain one major.minor.patch version, found {!r}".format(VERSION_FILE, text)
            )
        return text
    raise SetupError("{} is empty".format(VERSION_FILE))


def resolve_tool(tool: str) -> str:
    if os.path.isabs(tool) or tool.startswith(".") or os.sep in tool:
        return tool
    return shutil.which(tool) or tool


def version_command(tool: str) -> list:
    # Windows test/CI wrappers may be .bat/.cmd; cmd is the only way to execute them.
    if os.name == "nt" and Path(tool).suffix.lower() in (".bat", ".cmd"):
        return ["cmd", "/c", subprocess.list2cmdline([tool, "--version"])]
    return [tool, "--version"]


def query_version(tool: str, expected: str) -> tuple:
    resolved = resolve_tool(tool)
    try:
        result = subprocess.run(
            version_command(resolved),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
        )
    except FileNotFoundError:
        raise SetupError(
            "clang-format not found (looked for {!r}); install it with `{}` or set CLANG_FORMAT".format(
                tool, INSTALL_HINT.format(expected)
            )
        )
    except OSError as error:
        raise SetupError("cannot run {!r}: {}".format(tool, error))
    output = (result.stdout + result.stderr).strip()
    if result.returncode != 0:
        raise SetupError(
            "{} --version failed (exit {}): {}".format(tool, result.returncode, output or "<no output>")
        )
    match = VERSION_RE.search(output) or FALLBACK_VERSION_RE.search(output)
    if not match:
        raise SetupError(
            "cannot parse a clang-format version from {!r}: {}".format(tool, output or "<no output>")
        )
    return match.group(1), resolved


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)
    repo_root = Path(args.repo_root).resolve()

    payload = {
        "ok": False,
        "gate": "format-version",
        "expected": "",
        "found": "",
        "tool": os.environ.get("CLANG_FORMAT", "clang-format"),
        "exit_code": 2,
        "message": "",
    }
    try:
        expected = read_expected(repo_root)
        payload["expected"] = expected
        found, resolved = query_version(os.environ.get("CLANG_FORMAT") or "clang-format", expected)
        payload["found"] = found
        payload["tool"] = resolved
        if found == expected:
            payload["ok"] = True
            payload["exit_code"] = 0
            payload["message"] = "clang-format {} matches {}".format(found, VERSION_FILE)
            status = 0
        else:
            payload["exit_code"] = 1
            payload["message"] = (
                "clang-format version mismatch: CI pins {} ({}), found {} ({}); install it with "
                "`{}` or set CLANG_FORMAT to a matching binary".format(
                    expected, VERSION_FILE, found, resolved, INSTALL_HINT.format(expected)
                )
            )
            status = 1
    except SetupError as error:
        payload["message"] = str(error)
        status = 2

    if args.as_json:
        print(json.dumps(payload, indent=2, sort_keys=True))
    elif status == 0:
        print("format-version gate: {}".format(payload["message"]))
    else:
        print("error: {}".format(payload["message"]), file=sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(main())
