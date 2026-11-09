#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture-based self-tests for scripts/check_package.py.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

Archives are built on the fly from the license-bundle fixture tree, so the tests cover the same
production-linked license coverage logic the real packages use.
"""

from __future__ import annotations

import io
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from typing import Dict, Iterable, Optional

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
SCRIPT = SCRIPTS_DIR / "check_package.py"
FIXTURES = TESTS_DIR / "fixtures" / "licenses"

sys.path.insert(0, str(SCRIPTS_DIR))
import gen_license_bundle as bundle  # noqa: E402

DEPENDENCIES = FIXTURES / "dependencies.md"
NOTICES = FIXTURES / "notices.md"
TEXTS = FIXTURES / "text"
TOP_DIR = "tessera-0.0.1-linux-x86_64"

MINIMAL_SBOM = {
    "spdxVersion": "SPDX-2.3",
    "dataLicense": "CC0-1.0",
    "SPDXID": "SPDXRef-DOCUMENT",
    "name": "Tessera-0.0.1",
    "documentNamespace": "https://example.invalid/spdx/tessera-0.0.1",
    "creationInfo": {"created": "1970-01-01T00:00:00Z", "creators": ["Tool: fixture"]},
    "packages": [
        {
            "name": "Tessera",
            "SPDXID": "SPDXRef-Package-Tessera",
            "versionInfo": "0.0.1",
            "downloadLocation": "https://example.invalid/tessera",
            "filesAnalyzed": False,
            "licenseConcluded": "MIT",
            "licenseDeclared": "MIT",
            "copyrightText": "fixture",
        }
    ],
    "relationships": [],
}


def license_bundle_text() -> str:
    return bundle.render_bundle(
        DEPENDENCIES.read_text(encoding="utf-8"),
        NOTICES.read_text(encoding="utf-8"),
        TEXTS,
    )[0]


def package_info(
    version: str = "0.0.1",
    platform: str = "linux-x86_64",
    static_runtime: str = "ON",
    executables: str = "app",
    extra_lines: str = "",
) -> str:
    return (
        "Tessera package\n"
        "Version: {}\n"
        "Commit: abc123456789\n"
        "Platform: {}\n"
        "StaticRuntime: {}\n"
        "Executables: {}\n"
        "{}"
    ).format(version, platform, static_runtime, executables, extra_lines)


class PackageGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.dir = Path(self._temp.name)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def build_archive(
        self,
        *,
        omit: Iterable[str] = (),
        extra_files: Optional[Dict[str, bytes]] = None,
        info: Optional[str] = None,
        sbom: Optional[object] = None,
        sbom_raw: Optional[bytes] = None,
        bundle_text: Optional[str] = None,
        executable: str = "app",
        top_dir: str = TOP_DIR,
        archive_format: str = "tar",
        filename: str = "package.tar.gz",
    ) -> Path:
        files: Dict[str, bytes] = {
            "LICENSE": b"MIT fixture license\n",
            "THIRD_PARTY_NOTICES.md": b"# Notices (fixture)\n",
            "THIRD_PARTY_LICENSES.txt": (bundle_text or license_bundle_text()).encode("utf-8"),
            "sbom.spdx.json": (
                sbom_raw
                if sbom_raw is not None
                else json.dumps(sbom if sbom is not None else MINIMAL_SBOM).encode("utf-8")
            ),
            "PACKAGE-INFO.txt": (info or package_info()).encode("utf-8"),
            executable: b"",
        }
        for name in omit:
            files.pop(name, None)
        for name, content in (extra_files or {}).items():
            files[name] = content
        path = self.dir / filename
        if archive_format == "zip":
            with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
                for name, content in files.items():
                    member = zipfile.ZipInfo("{}/{}".format(top_dir, name))
                    member.external_attr = (0o755 if name == executable else 0o644) << 16
                    archive.writestr(member, content)
        else:
            with tarfile.open(path, "w:gz") as archive:
                for name, content in files.items():
                    member = tarfile.TarInfo("{}/{}".format(top_dir, name))
                    member.size = len(content)
                    member.mode = 0o755 if name == executable else 0o644
                    member.mtime = 0
                    archive.addfile(member, io.BytesIO(content))
        return path

    def run_check(self, archive: Path, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(SCRIPT),
                "--archive",
                str(archive),
                "--dependencies",
                str(DEPENDENCIES),
                *extra,
            ],
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
        )

    def test_clean_tar_package_passes(self) -> None:
        archive = self.build_archive()
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 error(s)", result.stdout)

    def test_clean_zip_package_passes(self) -> None:
        archive = self.build_archive(archive_format="zip", filename="package.zip")
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_sbom_is_an_error(self) -> None:
        archive = self.build_archive(omit=("sbom.spdx.json",))
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("missing required file 'sbom.spdx.json'", result.stdout)

    def test_malformed_sbom_is_an_error(self) -> None:
        archive = self.build_archive(sbom_raw=b"this is not json")
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("sbom.spdx.json: not valid JSON", result.stdout)

    def test_wrong_spdx_version_is_an_error(self) -> None:
        sbom = dict(MINIMAL_SBOM)
        sbom["spdxVersion"] = "SPDX-2.2"
        archive = self.build_archive(sbom=sbom)
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("expected 'SPDX-2.3'", result.stdout)

    def test_build_path_leak_is_an_error(self) -> None:
        archive = self.build_archive(
            info=package_info(extra_lines="BuildPath: D:/GitSrc/Tessera/build\n")
        )
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("absolute build path leaked", result.stdout)

    def test_timestamp_leak_is_an_error(self) -> None:
        archive = self.build_archive(
            info=package_info(extra_lines="Generated: 2026-01-01T12:00:00Z\n")
        )
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("build timestamp leaked", result.stdout)

    def test_stray_object_file_is_an_error(self) -> None:
        archive = self.build_archive(extra_files={"app.o": b"object"})
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("unexpected build artifact", result.stdout)

    def test_static_runtime_off_is_an_error(self) -> None:
        archive = self.build_archive(info=package_info(static_runtime="OFF"))
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ADR-012 requires a static C/C++ runtime", result.stdout)

    def test_missing_listed_executable_is_an_error(self) -> None:
        archive = self.build_archive(omit=("app",))
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("executable 'app'", result.stdout)

    def test_missing_license_text_is_an_error(self) -> None:
        bundle_text = license_bundle_text().replace(
            "License: IJG", "License: IJG (removed fixture section)"
        )
        archive = self.build_archive(bundle_text=bundle_text)
        result = self.run_check(archive)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("missing license text section for 'IJG'", result.stdout)

    def test_not_an_archive_is_a_validation_error(self) -> None:
        path = self.dir / "package.tar.gz"
        path.write_text("this is not an archive", encoding="utf-8")
        result = self.run_check(path)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not a package archive", result.stderr + result.stdout)

    def test_missing_archive_path_is_a_usage_error(self) -> None:
        result = self.run_check(self.dir / "does-not-exist.tar.gz")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_json_report(self) -> None:
        archive = self.build_archive()
        result = self.run_check(archive, "--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "package")
        self.assertEqual(payload["sbom_packages"], 1)
        self.assertEqual(payload["package"]["static_runtime"], "ON")

    def test_json_report_for_failure(self) -> None:
        archive = self.build_archive(omit=("sbom.spdx.json",))
        result = self.run_check(archive, "--json")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["ok"])
        self.assertTrue(any("sbom.spdx.json" in error for error in payload["errors"]))


if __name__ == "__main__":
    unittest.main()
