#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture-based self-tests for scripts/gen_sbom.py.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

The tests reuse the license-bundle fixture tree (small dependencies/notices/text set) and mutate
copies in a temporary directory.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
SCRIPT = SCRIPTS_DIR / "gen_sbom.py"
FIXTURES = TESTS_DIR / "fixtures" / "licenses"


class SbomGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.dir = Path(self._temp.name)
        self.dependencies = self.dir / "dependencies.md"
        self.notices = self.dir / "notices.md"
        self.output = self.dir / "sbom.spdx.json"
        shutil.copy(FIXTURES / "dependencies.md", self.dependencies)
        shutil.copy(FIXTURES / "notices.md", self.notices)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def run_sbom(self, *extra: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
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
            text=True,
            cwd=str(cwd or self.dir),
        )

    def write_and_load(self) -> dict:
        result = self.run_sbom("--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(self.output.read_text(encoding="utf-8"))

    def component(self, document: dict, name: str) -> dict:
        for package in document["packages"]:
            if package["name"] == name:
                return package
        self.fail("package {!r} not found".format(name))

    def test_document_shape_and_production_filtering(self) -> None:
        document = self.write_and_load()
        self.assertEqual(document["spdxVersion"], "SPDX-2.3")
        self.assertEqual(document["dataLicense"], "CC0-1.0")
        self.assertEqual(document["SPDXID"], "SPDXRef-DOCUMENT")
        self.assertTrue(document["name"].startswith("Tessera-"))
        self.assertTrue(document["documentNamespace"].startswith("https://"))
        self.assertIn("creationInfo", document)
        self.assertEqual(document["creationInfo"]["created"], "1970-01-01T00:00:00Z")
        self.assertEqual(document["creationInfo"]["creators"], ["Tool: tessera-gen-sbom"])
        names = [package["name"] for package in document["packages"]]
        # Project first, then the five production-linked fixture components, sorted.
        self.assertEqual(names[0], "Tessera")
        self.assertEqual(names[1:], sorted(names[1:], key=lambda name: (name.casefold(), name)))
        self.assertEqual(len(names), 6)
        self.assertNotIn("Zeta", names)  # test-only row
        self.assertNotIn("Eta", names)  # candidate row
        for package in document["packages"]:
            self.assertFalse(package["filesAnalyzed"])
            self.assertTrue(package["SPDXID"].startswith("SPDXRef-Package-"))
        self.assertEqual(
            len({package["SPDXID"].lower() for package in document["packages"]}), 6
        )
        relationships = document["relationships"]
        self.assertEqual(relationships[0]["relationshipType"], "DESCRIBES")
        self.assertEqual(
            {relationship["relationshipType"] for relationship in relationships},
            {"DESCRIBES", "DEPENDS_ON"},
        )
        self.assertEqual(len(relationships), 6)

    def test_dual_license_expression(self) -> None:
        document = self.write_and_load()
        self.assertEqual(
            self.component(document, "Beta")["licenseDeclared"], "BSD-3-Clause OR IJG"
        )
        self.assertEqual(self.component(document, "Gamma")["licenseDeclared"], "MIT-0")
        self.assertEqual(self.component(document, "Delta")["licenseDeclared"], "MIT")

    def test_public_domain_uses_license_ref(self) -> None:
        document = self.write_and_load()
        sqlite = self.component(document, "sqlite3")
        self.assertEqual(sqlite["licenseDeclared"], "LicenseRef-Public-Domain")
        infos = document["hasExtractedLicensingInfos"]
        self.assertEqual(len(infos), 1)
        self.assertEqual(infos[0]["licenseId"], "LicenseRef-Public-Domain")
        self.assertIn("PUBLIC DOMAIN FIXTURE TEXT", infos[0]["extractedText"])
        self.assertEqual(infos[0]["seeAlsos"], ["https://example.invalid/licenses/public-domain"])

    def test_unpinned_versions_are_omitted(self) -> None:
        document = self.write_and_load()
        for package in document["packages"]:
            if package["name"] == "Tessera":
                continue
            self.assertNotIn("versionInfo", package)

    def test_pinned_version_from_status(self) -> None:
        self.dependencies.write_text(
            self.dependencies.read_text(encoding="utf-8").replace(
                "| Alpha | V1 hard | MIT |", "| Alpha | V1 hard (vendored v9.9.9) | MIT |"
            ),
            encoding="utf-8",
        )
        document = self.write_and_load()
        self.assertEqual(self.component(document, "Alpha")["versionInfo"], "9.9.9")

    def test_upstream_url_and_version_from_notices(self) -> None:
        notices = self.notices.read_text(encoding="utf-8").replace(
            "- Copyright: Alpha Authors",
            "- Copyright: Alpha Authors\n- Upstream: https://example.invalid/alpha (v2.0.0)",
        )
        self.notices.write_text(notices, encoding="utf-8")
        document = self.write_and_load()
        alpha = self.component(document, "Alpha")
        self.assertEqual(alpha["versionInfo"], "2.0.0")
        self.assertEqual(alpha["downloadLocation"], "https://example.invalid/alpha")

    def test_byte_identical_across_runs_and_cwds(self) -> None:
        first = self.dir / "first.json"
        second = self.dir / "second.json"
        self.assertEqual(
            self.run_sbom("--write", "--output", str(first)).returncode, 0
        )
        self.assertEqual(
            self.run_sbom("--write", "--output", str(second), cwd=REPO_ROOT).returncode, 0
        )
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_check_detects_drift_without_rewriting(self) -> None:
        self.write_and_load()
        before = self.output.read_bytes()
        notices = self.notices.read_text(encoding="utf-8").replace(
            "Beta Authors", "Beta Authors (renamed)"
        )
        self.notices.write_text(notices, encoding="utf-8")
        result = self.run_sbom("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("stale", result.stderr)
        self.assertEqual(before, self.output.read_bytes())

    def test_check_passes_after_write(self) -> None:
        document = self.write_and_load()
        self.assertEqual(len(document["packages"]), 6)
        result = self.run_sbom("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("up to date", result.stdout)

    def test_missing_public_domain_text_fails(self) -> None:
        text_dir = self.dir / "text"
        shutil.copytree(FIXTURES / "text", text_dir)
        (text_dir / "Public-Domain-SQLite.txt").unlink()
        result = self.run_sbom("--write", "--licenses-dir", str(text_dir))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Public-Domain-SQLite.txt", result.stderr)
        self.assertFalse(self.output.exists())

    def test_missing_component_metadata_fails(self) -> None:
        notices = self.notices.read_text(encoding="utf-8").replace(
            "| sqlite3 | Public domain | SQLite authors |\n", ""
        )
        self.notices.write_text(notices, encoding="utf-8")
        result = self.run_sbom("--write")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no copyright holder", result.stderr)
        self.assertIn("sqlite3", result.stderr)

    def test_json_summary(self) -> None:
        result = self.run_sbom("--write", "--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "sbom")
        self.assertEqual(payload["spdx_version"], "SPDX-2.3")
        self.assertEqual(payload["package_count"], 6)
        self.assertEqual(payload["component_count"], 5)
        self.assertEqual(payload["extracted_license_count"], 1)
        self.assertGreater(payload["size_bytes"], 0)

    def test_no_absolute_paths_or_timestamps(self) -> None:
        self.write_and_load()
        text = self.output.read_text(encoding="utf-8")
        self.assertIn("1970-01-01T00:00:00Z", text)
        self.assertNotIn(str(REPO_ROOT), text)
        self.assertNotIn("generatedAt", text)

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
            text=True,
            cwd=str(self.dir),
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
