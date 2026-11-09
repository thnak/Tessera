#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture-based self-tests for scripts/gen_license_bundle.py.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
SCRIPT = SCRIPTS_DIR / "gen_license_bundle.py"
FIXTURES = TESTS_DIR / "fixtures" / "licenses"


class LicenseBundleTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.dir = Path(self._temp.name)
        self.dependencies = self.dir / "dependencies.md"
        self.notices = self.dir / "notices.md"
        self.output = self.dir / "THIRD_PARTY_LICENSES.txt"
        shutil.copy(FIXTURES / "dependencies.md", self.dependencies)
        shutil.copy(FIXTURES / "notices.md", self.notices)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def run_bundle(self, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(SCRIPT),
                "--dependencies",
                str(self.dependencies),
                "--notices",
                str(self.notices),
                "--licenses-dir",
                str(FIXTURES / "text"),
                "--output",
                str(self.output),
                *extra,
            ],
            capture_output=True,
            text=True, encoding="utf-8", errors="replace",
            cwd=str(self.dir),
        )

    def test_write_produces_sorted_deduped_bundle(self) -> None:
        result = self.run_bundle("--write")
        self.assertEqual(result.returncode, 0, result.stderr)
        content = self.output.read_text(encoding="utf-8")
        self.assertIn("Components (5)", content)
        self.assertIn("License texts (5)", content)
        # Each distinct license text appears exactly once, even with multiple users.
        self.assertEqual(content.count("MIT FIXTURE LICENSE TEXT"), 1)
        self.assertEqual(content.count("BSD-3-CLAUSE FIXTURE LICENSE TEXT"), 1)
        # Test-only and candidate rows are not part of the shipped set.
        map_section = content.split("License texts (")[0]
        self.assertNotIn("Zeta", map_section)
        self.assertNotIn("Eta", map_section)
        # Components are sorted case-insensitively.
        order = ["Alpha", "Beta", "Delta", "Gamma", "sqlite3"]
        positions = [map_section.index(name) for name in order]
        self.assertEqual(positions, sorted(positions))
        # License texts are sorted case-insensitively and deduplicated.
        text_section = content.split("License texts (", 1)[1]
        license_order = ["BSD-3-Clause", "IJG", "MIT", "MIT-0", "Public domain"]
        license_positions = [text_section.index("License: " + name) for name in license_order]
        self.assertEqual(license_positions, sorted(license_positions))
        # Public-domain dual licenses elect the permissive option.
        self.assertIn("Gamma\n  License: MIT-0", map_section)
        self.assertIn("Delta\n  License: MIT", map_section)

    def test_check_passes_after_write(self) -> None:
        self.assertEqual(self.run_bundle("--write").returncode, 0)
        result = self.run_bundle("--check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("up to date", result.stdout)

    def test_check_fails_on_stale_bundle(self) -> None:
        self.assertEqual(self.run_bundle("--write").returncode, 0)
        before = self.output.read_text(encoding="utf-8")
        notices = self.notices.read_text(encoding="utf-8").replace(
            "Beta Authors", "Beta Authors (renamed)"
        )
        self.notices.write_text(notices, encoding="utf-8")
        result = self.run_bundle("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("stale", result.stderr)
        # --check must not rewrite the stale bundle.
        self.assertEqual(before, self.output.read_text(encoding="utf-8"))

    def test_missing_license_text_fails(self) -> None:
        text_dir = self.dir / "text"
        shutil.copytree(FIXTURES / "text", text_dir)
        (text_dir / "IJG.txt").unlink()
        result = self.run_bundle("--write", "--licenses-dir", str(text_dir))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("IJG.txt", result.stderr)
        self.assertIn("Beta", result.stderr)
        self.assertFalse(self.output.exists())

    def test_missing_bundle_fails_check(self) -> None:
        result = self.run_bundle("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("missing", result.stderr)

    def test_unknown_public_domain_component_fails(self) -> None:
        with self.dependencies.open("a", encoding="utf-8") as handle:
            handle.write("| Omega | V1 hard | Public domain | `core` | no canonical text |\n")
        result = self.run_bundle("--write")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("public-domain", result.stderr)
        self.assertIn("Omega", result.stderr)

    def test_missing_copyright_fails(self) -> None:
        notices = self.notices.read_text(encoding="utf-8").replace(
            "| Beta | BSD-3-Clause / IJG | Beta Authors |\n", ""
        )
        self.notices.write_text(notices, encoding="utf-8")
        result = self.run_bundle("--write")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("copyright", result.stderr)
        self.assertIn("Beta", result.stderr)

    def test_license_text_requires_provenance_header(self) -> None:
        text_dir = self.dir / "text"
        shutil.copytree(FIXTURES / "text", text_dir)
        (text_dir / "MIT.txt").write_text(
            "MIT FIXTURE LICENSE TEXT without a header.\n", encoding="utf-8"
        )
        result = self.run_bundle("--write", "--licenses-dir", str(text_dir))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("provenance header", result.stderr)

    def test_missing_input_file_is_a_usage_error(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(SCRIPT),
                "--dependencies",
                str(self.dir / "nope.md"),
                "--check",
            ],
            capture_output=True,
            text=True, encoding="utf-8", errors="replace",
            cwd=str(self.dir),
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
