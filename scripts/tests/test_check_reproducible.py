#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture-based self-tests for scripts/check_reproducible.py.

The build phase is not exercised here (that needs a compiler); the tests cover the comparison and
diagnosis logic plus the setup-failure paths. Comparison fixtures are generated in a temp dir.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v
"""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
SCRIPT = SCRIPTS_DIR / "check_reproducible.py"


class ReproGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.dir = Path(self._temp.name)
        self.a = self.dir / "artifact-a.bin"
        self.b = self.dir / "artifact-b.bin"

    def tearDown(self) -> None:
        self._temp.cleanup()

    def run_compare(self, *extra: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(SCRIPT),
                "--compare",
                str(self.a),
                str(self.b),
                *extra,
            ],
            capture_output=True,
            text=True, encoding="utf-8", errors="replace",
            cwd=str(cwd or REPO_ROOT),
        )

    def write(self, data_a: bytes, data_b: bytes) -> None:
        self.a.write_bytes(data_a)
        self.b.write_bytes(data_b)

    def test_identical_artifacts_pass(self) -> None:
        data = bytes(range(256)) * 16
        self.write(data, data)
        result = self.run_compare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: artifacts are byte-identical", result.stdout)

    def test_one_byte_difference_fails_with_report(self) -> None:
        data_a = bytearray(bytes(range(256)) * 32)
        data_b = bytearray(data_a)
        offset = 2048
        data_b[offset] ^= 0xFF
        self.write(bytes(data_a), bytes(data_b))
        result = self.run_compare()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("FAIL: artifacts differ", result.stdout)
        self.assertIn("first at offset 0x800", result.stdout)
        self.assertIn("likely cause", result.stdout)
        self.assertIn("context A", result.stdout)

    def test_pe_timestamp_difference_is_classified(self) -> None:
        data_a = bytearray(0x200)
        data_a[0:2] = b"MZ"
        struct.pack_into("<I", data_a, 0x3C, 0x40)
        data_a[0x40:0x44] = b"PE\0\0"
        struct.pack_into("<I", data_a, 0x48, 0x11111111)
        data_b = bytearray(data_a)
        struct.pack_into("<I", data_b, 0x48, 0x22222222)
        self.write(bytes(data_a), bytes(data_b))
        result = self.run_compare("--json")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["classification"], "timestamp")
        self.assertIn("link timestamp", payload["detail"])

    def test_pe_timestamp_repeated_in_debug_directory_is_classified(self) -> None:
        data_a = bytearray(0x200)
        data_a[0:2] = b"MZ"
        struct.pack_into("<I", data_a, 0x3C, 0x40)
        data_a[0x40:0x44] = b"PE\0\0"
        struct.pack_into("<I", data_a, 0x48, 0x11111111)
        struct.pack_into("<I", data_a, 0x120, 0x11111111)
        data_b = bytearray(data_a)
        struct.pack_into("<I", data_b, 0x48, 0x22222222)
        struct.pack_into("<I", data_b, 0x120, 0x22222222)
        self.write(bytes(data_a), bytes(data_b))
        result = self.run_compare("--json")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["classification"], "timestamp")
        self.assertEqual(payload["regions"], 2)

    def test_absolute_path_difference_is_classified(self) -> None:
        data_a = bytearray(b"\x00" * 256)
        data_b = bytearray(data_a)
        marker_a = b"D:/build/reproducible/a/tests/unit/Release/merger_tests.exe"
        marker_b = b"D:/build/reproducible/b/tests/unit/Release/merger_tests.exe"
        data_a[64 : 64 + len(marker_a)] = marker_a
        data_b[64 : 64 + len(marker_b)] = marker_b
        self.write(bytes(data_a), bytes(data_b))
        result = self.run_compare("--json")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["classification"], "absolute-path")
        self.assertIn("prefix-map", payload["detail"])

    def test_ordering_difference_is_classified(self) -> None:
        data_a = bytearray(b"\x00" * 300)
        data_b = bytearray(data_a)
        data_a[100] = 1
        data_b[100] = 2
        data_a[250] = 3
        data_b[250] = 4
        self.write(bytes(data_a), bytes(data_b))
        result = self.run_compare("--json")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["classification"], "ordering")
        self.assertEqual(payload["regions"], 2)

    def test_missing_artifact_is_a_setup_error(self) -> None:
        self.a.write_bytes(b"only A exists")
        result = self.run_compare()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("artifact not found", result.stderr)

    def test_unavailable_cmake_is_a_setup_error(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(SCRIPT),
                "--cmake",
                str(self.dir / "does-not-exist-cmake"),
                "--build-root",
                str(self.dir / "repro"),
            ],
            capture_output=True,
            text=True, encoding="utf-8", errors="replace",
            cwd=str(REPO_ROOT),
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not found", result.stderr)

    def test_json_report_for_pass(self) -> None:
        data = b"identical artifact bytes"
        self.write(data, data)
        result = self.run_compare("--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "reproducible")
        self.assertTrue(payload["identical"])

    def test_cwd_independent(self) -> None:
        data = b"same"
        self.write(data, data)
        result = self.run_compare(cwd=Path(self._temp.name))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
