#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture-based self-tests for scripts/check_static_runtime.py.

The configure/build phase is not exercised here (CI runs it once); the tests cover the decision
logic that must work on any OS: Release vs Debug, missing artifact, missing tool output, and the
`/MT`-style vs dynamic-CRT import tables handed to the existing static-link gate.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
PROOF = SCRIPTS_DIR / "check_static_runtime.py"
FIXTURES = TESTS_DIR / "fixtures" / "static_link"


def run_proof(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(PROOF), *args],
        capture_output=True,
        text=True,
        cwd=str(REPO_ROOT),
    )


class StaticRuntimeProofTests(unittest.TestCase):
    def test_debug_config_is_skipped(self) -> None:
        result = run_proof("--config", "Debug")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SKIPPED", result.stdout)
        self.assertIn("Release", result.stdout)
        self.assertNotIn("PASS:", result.stdout)

    def test_debug_artifact_path_is_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "Debug" / "merger_tests.exe"
            result = run_proof("--skip-build", "--artifact", str(artifact))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SKIPPED", result.stdout)

    def test_release_missing_artifact_is_a_setup_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "missing" / "merger_tests"
            result = run_proof("--config", "Release", "--skip-build", "--artifact", str(artifact))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("artifact not found", result.stderr + result.stdout)

    def test_skip_build_requires_artifact(self) -> None:
        result = run_proof("--config", "Release", "--skip-build")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("--artifact", result.stderr + result.stdout)

    def test_mt_style_dumpbin_output_passes(self) -> None:
        result = run_proof(
            "--config", "Release", "--os", "windows", "--from-output", str(FIXTURES / "dumpbin_ok.txt")
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)

    def test_dynamic_msvc_runtime_is_a_violation(self) -> None:
        result = run_proof(
            "--config",
            "Release",
            "--os",
            "windows",
            "--from-output",
            str(FIXTURES / "dumpbin_vcruntime.txt"),
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("VCRUNTIME140.dll", result.stdout)
        self.assertIn("FAIL", result.stdout)

    def test_static_linux_output_passes(self) -> None:
        result = run_proof(
            "--config", "Release", "--os", "linux", "--from-output", str(FIXTURES / "ldd_ok.txt")
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_dynamic_libstdcxx_is_a_violation(self) -> None:
        result = run_proof(
            "--config",
            "Release",
            "--os",
            "linux",
            "--from-output",
            str(FIXTURES / "ldd_libstdcxx.txt"),
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("libstdc++", result.stdout)

    def test_missing_tool_is_a_setup_error(self) -> None:
        result = run_proof(
            "--config",
            "Release",
            "--skip-build",
            "--artifact",
            str(FIXTURES / "dumpbin_ok.txt"),
            "--os",
            "windows",
            "--dumpbin",
            str(FIXTURES / "does-not-exist-dumpbin.exe"),
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("dumpbin", (result.stdout + result.stderr).lower())

    def test_missing_tool_output_is_a_setup_error(self) -> None:
        result = run_proof(
            "--config", "Release", "--os", "linux", "--from-output", str(FIXTURES / "nope.txt")
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("tool output not found", result.stderr + result.stdout)


if __name__ == "__main__":
    unittest.main()
