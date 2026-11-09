#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture-based self-tests for scripts/check_pins.py.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v
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
SCRIPT = SCRIPTS_DIR / "check_pins.py"
FIXTURES = TESTS_DIR / "fixtures" / "pins"
DEPENDENCIES = FIXTURES / "dependencies.md"


class PinGateTests(unittest.TestCase):
    def run_case(self, case: str, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(SCRIPT),
                "--repo-root",
                str(FIXTURES / case),
                "--dependencies",
                str(DEPENDENCIES),
                *extra,
            ],
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
        )

    def test_clean_tree_passes(self) -> None:
        result = self.run_case("clean")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("alpha kind=url-sha256", result.stdout)
        self.assertIn("delta kind=git-commit", result.stdout)
        self.assertIn("gamma kind=vendored-source", result.stdout)
        self.assertIn("3 fetch(es), 3 pinned, 0 error(s)", result.stdout)
        self.assertNotIn("commented", result.stdout)

    def test_branch_pin_fails(self) -> None:
        result = self.run_case("branch")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ERROR", result.stdout)
        self.assertIn("GIT_TAG 'main' is not a full 40-hex commit SHA", result.stdout)

    def test_short_tag_pin_fails(self) -> None:
        result = self.run_case("short_tag")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("GIT_TAG 'v1.2.3' is not a full 40-hex commit SHA", result.stdout)

    def test_url_without_hash_fails(self) -> None:
        result = self.run_case("url_no_hash")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("URL fetch has no URL_HASH", result.stdout)

    def test_url_with_sha256_passes(self) -> None:
        result = self.run_case("url_hash")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("beta kind=url-sha256", result.stdout)

    def test_option_gated_unpinned_fetch_still_fails(self) -> None:
        result = self.run_case("option_gated")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("GIT_TAG 'main' is not a full 40-hex commit SHA", result.stdout)

    def test_fetch_without_dependency_row_fails(self) -> None:
        result = self.run_case("missing_row")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("newdep: fetched component has no row in docs/specs/dependencies.md", result.stdout)

    def test_make_available_without_declaration_fails(self) -> None:
        result = self.run_case("make_available_only")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("FetchContent_MakeAvailable(ghost)", result.stdout)

    def test_json_report(self) -> None:
        result = self.run_case("clean", "--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "pins")
        self.assertEqual(len(payload["pins"]), 3)
        self.assertEqual(payload["errors"], [])

    def test_missing_dependency_map_is_a_usage_error(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(SCRIPT),
                "--repo-root",
                str(FIXTURES / "clean"),
                "--dependencies",
                str(FIXTURES / "does-not-exist.md"),
            ],
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
