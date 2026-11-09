#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture tests for scripts/gates.py (the one-command gate aggregator).

Each test builds a tiny temp repository whose scripts/ directory contains stand-in gate scripts,
so the aggregator's ordering, filtering, summarising and failure semantics are covered without
running the real (slower) gates.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
GATES = SCRIPTS_DIR / "gates.py"

# Aggregator gate name -> stand-in script file name.
GATE_FILES = {
    "dependencies": "check_dependencies.py",
    "license-bundle": "gen_license_bundle.py",
    "sbom": "gen_sbom.py",
    "docs": "check_docs.py",
    "pins": "check_pins.py",
}
PACKAGE_FILE = "check_package.py"


class AggregatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name) / "repo"
        (self.root / "scripts").mkdir(parents=True)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write_stub(self, filename: str, exit_code: int = 0, message: str = "") -> None:
        script = (
            "import sys\n"
            "print({message!r})\n"
            "sys.exit({code})\n"
        ).format(message=message or "ran {}".format(filename), code=exit_code)
        (self.root / "scripts" / filename).write_text(script, encoding="utf-8")

    def build_repo(self, exits: dict | None = None, omit: tuple = (), message: str = "") -> None:
        exits = exits or {}
        for name, filename in GATE_FILES.items():
            if filename in omit:
                continue
            self.write_stub(filename, exits.get(name, 0), message=message)

    def run_gates(self, *extra: str, cwd: Path | None = None, env: dict | None = None):
        environment = dict(os.environ)
        if env:
            environment.update(env)
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(GATES),
                "--repo-root",
                str(self.root),
                *extra,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=str(cwd or REPO_ROOT),
            env=environment,
        )

    def test_all_gates_pass_with_summary(self) -> None:
        self.build_repo()
        result = self.run_gates()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("5 gate(s): 5 passed, 0 failed, 0 setup error(s)", result.stdout)
        for name in GATE_FILES:
            self.assertIn(name, result.stdout)

    def test_failing_gate_is_named_and_exits_1(self) -> None:
        self.build_repo(exits={"docs": 3}, message="docs-boom")
        result = self.run_gates()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("docs-boom", result.stdout)
        self.assertIn("docs", result.stdout)
        self.assertIn("4 passed, 1 failed", result.stdout)

    def test_missing_gate_script_is_a_setup_error(self) -> None:
        self.build_repo(omit=("gen_sbom.py",))
        result = self.run_gates()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("setup error", (result.stdout + result.stderr).lower())
        self.assertIn("gen_sbom.py", result.stderr)
        self.assertIn("1 setup error(s)", result.stdout)

    def test_only_runs_selected_gates_in_listed_order(self) -> None:
        self.build_repo(message="ran-gate")
        result = self.run_gates("--only", "pins,docs")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("2 gate(s): 2 passed", result.stdout)
        self.assertIn("docs", result.stdout)
        self.assertIn("pins", result.stdout)
        self.assertNotIn("dependencies", result.stdout)
        self.assertLess(result.stdout.index("Gate summary"), len(result.stdout))

    def test_only_with_unknown_gate_is_a_usage_error(self) -> None:
        self.build_repo()
        result = self.run_gates("--only", "nope")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("unknown gate", result.stderr)

    def test_list_does_not_run_gates(self) -> None:
        self.build_repo()
        marker = self.root / "scripts" / "ran.marker"
        (self.root / "scripts" / "check_docs.py").write_text(
            "from pathlib import Path\nPath({!r}).write_text('ran')\n".format(str(marker)),
            encoding="utf-8",
        )
        result = self.run_gates("--list")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("dependencies", result.stdout)
        self.assertIn("pins", result.stdout)
        self.assertIn("package", result.stdout)
        self.assertIn("fixtures", result.stdout)
        self.assertFalse(marker.exists())

    def test_json_report_for_pass(self) -> None:
        self.build_repo()
        result = self.run_gates("--json", "--quiet")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "aggregator")
        self.assertEqual(len(payload["gates"]), 5)
        self.assertTrue(all(gate["status"] == "pass" for gate in payload["gates"]))
        self.assertEqual(payload["exit_code"], 0)

    def test_json_report_for_failure(self) -> None:
        self.build_repo(exits={"sbom": 1}, message="sbom-broke")
        result = self.run_gates("--json", "--quiet")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["ok"])
        failing = [gate for gate in payload["gates"] if gate["status"] == "fail"]
        self.assertEqual([gate["name"] for gate in failing], ["sbom"])
        self.assertIn("sbom-broke", failing[0]["output"])

    def test_archive_adds_package_gate(self) -> None:
        self.build_repo()
        self.write_stub(PACKAGE_FILE, 0, "package-ran")
        archive = self.root / "package.tar.gz"
        archive.write_bytes(b"not really an archive")
        result = self.run_gates("--archive", str(archive))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("6 gate(s): 6 passed", result.stdout)

    def test_cp1252_console_does_not_crash(self) -> None:
        self.build_repo(message="caf\u00e9 \u00a7 unicode output")
        result = self.run_gates(env={"PYTHONIOENCODING": "cp1252"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("UnicodeEncodeError", result.stderr)
        self.assertIn("5 gate(s): 5 passed", result.stdout)

    def test_cwd_independent(self) -> None:
        self.build_repo()
        result = self.run_gates(cwd=self.root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
