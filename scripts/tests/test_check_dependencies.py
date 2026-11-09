#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture-based self-tests for scripts/check_dependencies.py.

Run from the repository root with the standard library only:

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

The tests drive the real command line, so exit codes and the human-readable report are covered.
"""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
CHECK = SCRIPTS_DIR / "check_dependencies.py"
FIXTURES = TESTS_DIR / "fixtures" / "dependencies"
NOTICES = FIXTURES / "valid_notices.md"


def run_gate(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(CHECK), *args],
        capture_output=True,
        text=True,
        cwd=str(REPO_ROOT),
    )


class DependencyGateTests(unittest.TestCase):
    def check_fixture(self, name: str) -> subprocess.CompletedProcess:
        return run_gate("--dependencies", str(FIXTURES / name), "--notices", str(NOTICES))

    def test_valid_fixture_passes(self) -> None:
        result = self.check_fixture("valid_dependencies.md")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)
        self.assertIn("duplicate row", result.stdout)
        self.assertIn("non-third-party", result.stdout)
        self.assertIn("does not match a dependency-table row", result.stdout)

    def test_missing_notice_fails(self) -> None:
        result = self.check_fixture("missing_notice_dependencies.md")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("MISSING_NOTICE", result.stdout)
        self.assertIn("Omega", result.stdout)

    def test_gpl_in_shipped_code_fails(self) -> None:
        result = self.check_fixture("forbidden_license_dependencies.md")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("FORBIDDEN_LICENSE", result.stdout)
        self.assertIn("Alpha", result.stdout)

    def test_gpl_candidate_without_exemption_fails(self) -> None:
        result = self.check_fixture("forbidden_candidate_dependencies.md")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("FORBIDDEN_LICENSE", result.stdout)

    def test_unknown_status_fails(self) -> None:
        result = self.check_fixture("unknown_status_dependencies.md")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("UNKNOWN_STATUS", result.stdout)
        self.assertIn("V2 maybe", result.stdout)

    def test_conflicting_duplicate_fails(self) -> None:
        result = self.check_fixture("duplicate_conflict_dependencies.md")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("DUPLICATE_CONFLICT", result.stdout)

    def test_disallowed_license_fails(self) -> None:
        result = self.check_fixture("disallowed_license_dependencies.md")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("LICENSE_NOT_ALLOWED", result.stdout)
        self.assertIn("SSPL-1.0", result.stdout)

    def test_missing_license_fails(self) -> None:
        result = self.check_fixture("missing_license_dependencies.md")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("MISSING_LICENSE", result.stdout)

    def test_json_report_is_machine_readable(self) -> None:
        result = run_gate(
            "--dependencies",
            str(FIXTURES / "missing_notice_dependencies.md"),
            "--notices",
            str(NOTICES),
            "--json",
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["gate"], "dependency-license")
        self.assertEqual(len(payload["violations"]), 1)
        self.assertEqual(payload["violations"][0]["kind"], "MISSING_NOTICE")
        self.assertEqual(payload["violations"][0]["component"], "Omega")

    def test_json_report_for_valid_fixture(self) -> None:
        result = run_gate(
            "--dependencies",
            str(FIXTURES / "valid_dependencies.md"),
            "--notices",
            str(NOTICES),
            "--json",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["violations"], [])
        self.assertGreater(payload["production_linked"], 0)

    def test_missing_file_is_a_usage_error(self) -> None:
        result = run_gate(
            "--dependencies",
            str(FIXTURES / "does-not-exist.md"),
            "--notices",
            str(NOTICES),
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
