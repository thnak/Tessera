#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture tests for scripts/check_format_version.py.

Each test builds a tiny temp repository with a pinned version file and fake clang-format wrappers,
so the gate's pass/mismatch/setup semantics are covered without needing a real formatter.
"""

from __future__ import annotations

import json
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
CHECK = SCRIPTS_DIR / "check_format_version.py"
PINNED = "22.1.3"


class FormatVersionTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name) / "repo"
        (self.root / "tools").mkdir(parents=True)
        self.bin = Path(self._temp.name) / "bin"
        self.bin.mkdir()
        self.write_pin(PINNED)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write_pin(self, text: str) -> None:
        (self.root / "tools" / "format-version.txt").write_text(text + "\n", encoding="utf-8")

    def make_tool(self, output: str, name: str = "clang-format") -> Path:
        if os.name == "nt":
            tool = self.bin / (name + ".bat")
            tool.write_text("@echo {}\n".format(output), encoding="ascii")
        else:
            tool = self.bin / name
            tool.write_text("#!/bin/sh\necho {}\n".format(output), encoding="ascii")
            tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
        return tool

    def run_gate(self, tool: Path, json_mode: bool = False) -> subprocess.CompletedProcess:
        environment = dict(os.environ)
        if tool is not None:
            environment["CLANG_FORMAT"] = str(tool)
        command = [sys.executable, "-B", str(CHECK), "--repo-root", str(self.root)]
        if json_mode:
            command.append("--json")
        return subprocess.run(
            command,
            cwd=str(REPO_ROOT),
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

    def test_matching_version_passes(self) -> None:
        result = self.run_gate(self.make_tool("clang-format version " + PINNED))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("matches", result.stdout)

    def test_version_mismatch_fails_with_actionable_message(self) -> None:
        result = self.run_gate(self.make_tool("clang-format version 22.1.5"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("clang-format version mismatch", result.stderr)
        self.assertIn("CI pins " + PINNED, result.stderr)
        self.assertIn("found 22.1.5", result.stderr)
        self.assertIn("pip install --user clang-format==" + PINNED, result.stderr)

    def test_missing_tool_is_a_setup_error(self) -> None:
        result = self.run_gate(self.bin / "not-installed")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("clang-format not found", result.stderr)
        self.assertIn("CLANG_FORMAT", result.stderr)

    def test_clang_format_override_is_honoured(self) -> None:
        newer = self.make_tool("clang-format version 22.1.5", name="newer")
        pinned = self.make_tool("clang-format version " + PINNED, name="pinned")
        failed = self.run_gate(newer)
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        passed = self.run_gate(pinned)
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)

    def test_json_reports_match(self) -> None:
        result = self.run_gate(self.make_tool("clang-format version " + PINNED), json_mode=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "format-version")
        self.assertEqual(payload["expected"], PINNED)
        self.assertEqual(payload["found"], PINNED)
        self.assertEqual(payload["exit_code"], 0)

    def test_json_reports_mismatch(self) -> None:
        result = self.run_gate(self.make_tool("clang-format version 22.1.5"), json_mode=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["expected"], PINNED)
        self.assertEqual(payload["found"], "22.1.5")
        self.assertEqual(payload["exit_code"], 1)

    def test_missing_version_file_is_a_setup_error(self) -> None:
        (self.root / "tools" / "format-version.txt").unlink()
        result = self.run_gate(self.make_tool("clang-format version " + PINNED))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("cannot read", result.stderr)

    def test_malformed_pin_is_a_setup_error(self) -> None:
        self.write_pin("22.1")
        result = self.run_gate(self.make_tool("clang-format version " + PINNED))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("major.minor.patch", result.stderr)

    def test_unparseable_version_is_a_setup_error(self) -> None:
        result = self.run_gate(self.make_tool("not a formatter"))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("cannot parse", result.stderr)


if __name__ == "__main__":
    unittest.main()
