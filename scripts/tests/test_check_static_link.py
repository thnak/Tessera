#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture-based self-tests for scripts/check_static_link.py.

The tests feed synthetic ``ldd``/``otool -L``/``dumpbin /dependents`` output through
``--from-output`` so they run on Windows, Linux and macOS without the real tools installed:

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
CHECK = SCRIPTS_DIR / "check_static_link.py"
FIXTURES = TESTS_DIR / "fixtures" / "static_link"


def run_gate(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(CHECK), *args],
        capture_output=True,
        text=True,
        cwd=str(REPO_ROOT),
    )


class StaticLinkGateTests(unittest.TestCase):
    def check_output(self, os_name: str, name: str, *extra: str) -> subprocess.CompletedProcess:
        return run_gate(
            "--os",
            os_name,
            "--from-output",
            str(FIXTURES / name),
            *extra,
        )

    def test_linux_allowlisted_dependencies_pass(self) -> None:
        result = self.check_output("linux", "ldd_ok.txt")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)
        self.assertIn("libvulkan.so.1", result.stdout)

    def test_linux_static_binary_passes(self) -> None:
        result = self.check_output("linux", "ldd_static.txt")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("statically linked", result.stdout)

    def test_dynamic_libstdcxx_fails(self) -> None:
        result = self.check_output("linux", "ldd_libstdcxx.txt")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("libstdc++", result.stdout)
        self.assertIn("libgcc_s", result.stdout)
        self.assertIn("forbidden", result.stdout)

    def test_unexpected_third_party_library_fails(self) -> None:
        result = self.check_output("linux", "ldd_unexpected.txt")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("libpng16.so.16", result.stdout)
        self.assertIn("not in the linux allowlist", result.stdout)

    def test_unresolved_dependency_fails(self) -> None:
        result = self.check_output("linux", "ldd_not_found.txt")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("libfoo.so.1", result.stdout)
        self.assertIn("unresolved", result.stdout)

    def test_macos_system_frameworks_pass(self) -> None:
        result = self.check_output("macos", "otool_ok.txt")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("libSystem.B.dylib", result.stdout)
        self.assertIn("libc++.1.dylib", result.stdout)

    def test_macos_homebrew_library_fails(self) -> None:
        result = self.check_output("macos", "otool_unexpected.txt")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("libpng16.16.dylib", result.stdout)

    def test_macos_dynamic_libgcc_fails(self) -> None:
        result = self.check_output("macos", "otool_libgcc.txt")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("libgcc_s", result.stdout)
        self.assertIn("forbidden", result.stdout)

    def test_windows_allowlisted_dependencies_pass(self) -> None:
        result = self.check_output("windows", "dumpbin_ok.txt")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("KERNEL32.dll", result.stdout)
        self.assertIn("vulkan-1.dll", result.stdout)

    def test_windows_dynamic_vcruntime_fails(self) -> None:
        result = self.check_output("windows", "dumpbin_vcruntime.txt")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("VCRUNTIME140.dll", result.stdout)
        self.assertIn("MSVCP140.dll", result.stdout)
        self.assertIn("forbidden", result.stdout)

    def test_windows_unexpected_dll_fails(self) -> None:
        result = self.check_output("windows", "dumpbin_unexpected.txt")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("libpng16.dll", result.stdout)

    def test_allowlist_override_passes_extra_library(self) -> None:
        result = self.check_output(
            "linux",
            "ldd_unexpected.txt",
            "--allowlist",
            str(FIXTURES / "allowlist_override.txt"),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_allowlist_cannot_unforbid_dynamic_cpp_runtime(self) -> None:
        result = self.check_output(
            "linux",
            "ldd_libstdcxx.txt",
            "--allowlist",
            str(FIXTURES / "allowlist_stdlib.txt"),
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("forbidden", result.stdout)

    def test_json_report_is_machine_readable(self) -> None:
        result = self.check_output("linux", "ldd_unexpected.txt", "--json")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["gate"], "static-link")
        self.assertEqual(payload["platform"], "linux")
        self.assertEqual(payload["violation_count"], 1)
        denied = [lib for lib in payload["binaries"][0]["libraries"] if not lib["allowed"]]
        self.assertEqual(denied[0]["name"], "libpng16.so.16")

    def test_missing_output_file_is_a_usage_error(self) -> None:
        result = run_gate(
            "--os", "linux", "--from-output", str(FIXTURES / "does-not-exist.txt")
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_unavailable_tool_is_a_setup_error(self) -> None:
        result = run_gate(
            "--os",
            "windows",
            "--binary",
            str(CHECK),
            "--dumpbin",
            str(FIXTURES / "does-not-exist-dumpbin.exe"),
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("dumpbin", (result.stdout + result.stderr).lower())


if __name__ == "__main__":
    unittest.main()
