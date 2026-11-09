#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture tests for ``check_reproducible.py --package``.

Fake ``cmake``/``cpack`` wrappers stand in for the real toolchain: the build step creates a dummy
artifact and the cpack step writes an archive whose bytes record the reproducibility environment.
This covers archive discovery, the fixed SOURCE_DATE_EPOCH/TZ defaults, the override flags and the
pass/mismatch exit codes without a real build.
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
CHECK = SCRIPTS_DIR / "check_reproducible.py"

CMAKE_STUB = '''\
import pathlib
import sys

args = sys.argv[1:]


def value(flag):
    return args[args.index(flag) + 1] if flag in args else None


if "-B" in args:
    pathlib.Path(value("-B")).mkdir(parents=True, exist_ok=True)
if "--build" in args:
    build = pathlib.Path(value("--build"))
    target = value("--target") or "merger_tests"
    artifact = build / "tests" / "unit" / (target + (".exe" if sys.platform == "win32" else ""))
    artifact.parent.mkdir(parents=True, exist_ok=True)
    artifact.write_bytes(b"fake executable")
sys.exit(0)
'''

CPACK_STUB = '''\
import os
import pathlib
import sys

args = sys.argv[1:]
out = pathlib.Path(args[args.index("-B") + 1])
out.mkdir(parents=True, exist_ok=True)
suffix = ".zip" if sys.platform == "win32" else ".tar.gz"
fields = [
    "SOURCE_DATE_EPOCH=" + os.environ.get("SOURCE_DATE_EPOCH", "<unset>"),
    "TZ=" + os.environ.get("TZ", "<unset>"),
]
if os.environ.get("CPACK_STUB_VARY"):
    fields.append("vary=" + str(out))
(out / ("tessera-0.0.0-fixture" + suffix)).write_bytes("\\n".join(fields).encode("utf-8"))
sys.exit(0)
'''


class ReproduciblePackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.root / "repo").mkdir()
        self._write_tool("cmake", CMAKE_STUB)
        self._write_tool("cpack", CPACK_STUB)
        self.build_root = self.root / "repro"

    def tearDown(self) -> None:
        self._temp.cleanup()

    def _write_tool(self, name: str, body: str) -> None:
        implementation = self.bin / (name + "_impl.py")
        implementation.write_text(body, encoding="utf-8")
        if os.name == "nt":
            wrapper = self.bin / (name + ".bat")
            wrapper.write_text('@python "%~dp0{}_impl.py" %*\n'.format(name), encoding="ascii")
        else:
            wrapper = self.bin / name
            wrapper.write_text(
                '#!/bin/sh\nexec python "$(dirname "$0")/{}_impl.py" "$@"\n'.format(name),
                encoding="ascii",
            )
            wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)

    def run_gate(self, *extra: str, env: dict | None = None) -> subprocess.CompletedProcess:
        environment = dict(os.environ)
        environment["PATH"] = str(self.bin) + os.pathsep + environment.get("PATH", "")
        environment.pop("SOURCE_DATE_EPOCH", None)
        environment.pop("TZ", None)
        if env:
            environment.update(env)
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(CHECK),
                "--package",
                "--repo-root",
                str(self.root / "repo"),
                "--build-root",
                str(self.build_root),
                "--cmake",
                str(self.bin / ("cmake.bat" if os.name == "nt" else "cmake")),
                "--cpack",
                str(self.bin / ("cpack.bat" if os.name == "nt" else "cpack")),
                "--target",
                "merger_tests",
                "--commit",
                "deadbeef1234",
                *extra,
            ],
            cwd=str(REPO_ROOT),
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

    def test_package_archives_are_byte_identical_with_fixed_epoch(self) -> None:
        result = self.run_gate("--json", "--keep")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "reproducible-package")
        self.assertEqual(payload["mode"], "package")
        archive = Path(payload["artifact_a"]["path"])
        self.assertEqual(archive.name, "tessera-0.0.0-fixture.tar.gz" if os.name != "nt" else "tessera-0.0.0-fixture.zip")
        data = archive.read_bytes().decode("utf-8")
        self.assertIn("SOURCE_DATE_EPOCH=315532800", data)
        self.assertIn("TZ=UTC", data)
        self.assertEqual(payload["artifact_a"]["sha256"], payload["artifact_b"]["sha256"])

    def test_env_overrides_reach_cpack(self) -> None:
        result = self.run_gate(
            "--json",
            "--keep",
            "--source-date-epoch",
            "1234567890",
            "--archive-timezone",
            "Asia/Saigon",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        data = Path(payload["artifact_a"]["path"]).read_bytes().decode("utf-8")
        self.assertIn("SOURCE_DATE_EPOCH=1234567890", data)
        self.assertIn("TZ=Asia/Saigon", data)

    def test_archive_mismatch_fails(self) -> None:
        result = self.run_gate("--json", env={"CPACK_STUB_VARY": "1"})
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["ok"])
        self.assertFalse(payload["identical"])

    def test_missing_cpack_is_a_setup_error(self) -> None:
        environment = dict(os.environ)
        environment["PATH"] = str(self.bin) + os.pathsep + environment.get("PATH", "")
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(CHECK),
                "--package",
                "--repo-root",
                str(self.root / "repo"),
                "--build-root",
                str(self.build_root),
                "--cmake",
                str(self.bin / ("cmake.bat" if os.name == "nt" else "cmake")),
                "--cpack",
                str(self.bin / "cpack-not-installed"),
                "--target",
                "merger_tests",
                "--commit",
                "deadbeef1234",
            ],
            cwd=str(REPO_ROOT),
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("cpack-not-installed", result.stderr)


if __name__ == "__main__":
    unittest.main()
