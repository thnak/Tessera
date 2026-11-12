#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture tests for scripts/ci/validate_wgsl.py.

A fake naga reports a version and rejects any file containing BROKEN, so the gate's pass, failure,
version-pin and setup semantics are covered without the real tool.
"""

from __future__ import annotations

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
CHECK = SCRIPTS_DIR / "ci" / "validate_wgsl.py"
PINNED = "30.0.1"


class ValidateWgslTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name) / "repo"
        (self.root / "tools").mkdir(parents=True)
        (self.root / "src" / "shaders").mkdir(parents=True)
        (self.root / "tools" / "naga-version.txt").write_text(PINNED + "\n", encoding="utf-8")
        self.bin = Path(self._temp.name) / "bin"
        self.bin.mkdir()

    def tearDown(self) -> None:
        self._temp.cleanup()

    def make_naga(self, version: str) -> Path:
        if os.name == "nt":
            tool = self.bin / "naga.bat"
            tool.write_text(
                "@echo off\r\n"
                'if "%~1"=="--version" (echo {}& exit /b 0)\r\n'
                'findstr /c:"BROKEN" "%~1" >nul && (echo error: invalid& exit /b 1)\r\n'
                "exit /b 0\r\n".format(version),
                encoding="ascii",
            )
        else:
            tool = self.bin / "naga"
            tool.write_text(
                "#!/bin/sh\n"
                'if [ "$1" = "--version" ]; then echo {}; exit 0; fi\n'
                'if grep -q BROKEN "$1"; then echo "error: invalid"; exit 1; fi\n'
                "exit 0\n".format(version),
                encoding="ascii",
            )
            tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
        return tool

    def write_shader(self, name: str, text: str) -> None:
        (self.root / "src" / "shaders" / name).write_text(text, encoding="utf-8")

    def run_gate(self, naga: Path, *extra: str) -> subprocess.CompletedProcess:
        command = [sys.executable, "-B", str(CHECK), "--repo-root", str(self.root)]
        command += ["--naga", str(naga), *extra]
        return subprocess.run(
            command, cwd=str(REPO_ROOT), capture_output=True, text=True, encoding="utf-8"
        )

    def test_valid_shaders_pass(self) -> None:
        self.write_shader("a.wgsl", "fn main() {}\n")
        self.write_shader("b.wgsl", "fn other() {}\n")
        result = self.run_gate(self.make_naga(PINNED))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("2 WGSL file(s), 0 failed", result.stdout)

    def test_invalid_shader_fails(self) -> None:
        self.write_shader("a.wgsl", "fn main() {}\n")
        self.write_shader("bad.wgsl", "BROKEN\n")
        result = self.run_gate(self.make_naga(PINNED))
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL src/shaders/bad.wgsl", result.stdout)

    def test_version_mismatch_fails_unless_allowed(self) -> None:
        self.write_shader("a.wgsl", "fn main() {}\n")
        naga = self.make_naga("29.0.0")
        self.assertEqual(self.run_gate(naga).returncode, 1)
        self.assertEqual(self.run_gate(naga, "--allow-version-mismatch").returncode, 0)

    def test_missing_tool_or_files_is_setup_error(self) -> None:
        self.assertEqual(self.run_gate(self.bin / "absent").returncode, 2)
        self.assertEqual(self.run_gate(self.make_naga(PINNED)).returncode, 2)


if __name__ == "__main__":
    unittest.main()
