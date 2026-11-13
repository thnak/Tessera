#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-tests for scripts/golden_tolerances.py against the real tolerance file.

    python -B -m unittest discover -s scripts/tests -p "test_golden_tolerances.py" -v

The suite reads the checked-in ``tests/golden/tolerance.json`` and the real
``tests/gpu/CMakeLists.txt``, so it fails when a golden registration has no entry in the file and
no working fallback to ``defaults``.
"""

from __future__ import annotations

import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
SCRIPT = SCRIPTS_DIR / "golden_tolerances.py"
GPU_CMAKE = REPO_ROOT / "tests" / "gpu" / "CMakeLists.txt"
TOLERANCE_FILE = REPO_ROOT / "tests" / "golden" / "tolerance.json"

ADD_TEST = re.compile(r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_]+)\b")


def load_module():
    spec = importlib.util.spec_from_file_location("golden_tolerances", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


golden_tolerances = load_module()


def golden_test_names() -> list[str]:
    """Golden comparisons registered by tests/gpu/CMakeLists.txt, sorted."""

    text = GPU_CMAKE.read_text(encoding="utf-8")
    names = set()
    for match in ADD_TEST.finditer(text):
        name = match.group(1)
        # Golden image comparisons plus the harness baseline capture carry per-test tolerances; the
        # smoke/validation entries in the same file do not.
        if "golden" in name or name == "harness_frame_capture":
            names.add(name)
    return sorted(names)


def run_script(*arguments: str, path: Path | None = None) -> subprocess.CompletedProcess:
    command = [sys.executable, "-B", str(SCRIPT)]
    if path is not None:
        command += ["--path", str(path)]
    command += list(arguments)
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        cwd=str(REPO_ROOT),
    )


class GoldenToleranceTests(unittest.TestCase):
    def test_real_file_validates(self) -> None:
        document = golden_tolerances.load(TOLERANCE_FILE)
        self.assertEqual(document["schema"], 1)
        self.assertEqual(document["defaults"], {"tolerance": 0, "max_mismatch": 0.0})
        self.assertIn("batched2d_golden", document["tests"])

    def test_every_golden_test_name_resolves(self) -> None:
        document = golden_tolerances.load(TOLERANCE_FILE)
        names = golden_test_names()
        self.assertGreaterEqual(len(names), 5)
        for required in (
            "batched2d_golden",
            "batched2d_overlay_golden",
            "batched2d_cpu_golden",
            "cpu_wide_golden",
            "harness_frame_capture",
        ):
            self.assertIn(required, names)
        for name in names:
            tolerance, max_mismatch = golden_tolerances.resolve(document, name)
            self.assertIsInstance(tolerance, int)
            self.assertGreaterEqual(tolerance, 0)
            self.assertGreaterEqual(max_mismatch, 0.0)
            if name not in document["tests"]:
                self.assertEqual(
                    (tolerance, max_mismatch),
                    (document["defaults"]["tolerance"], document["defaults"]["max_mismatch"]),
                    f"{name} has no entry and does not fall back to defaults",
                )

    def test_golden_name_detection_ignores_non_golden_entries(self) -> None:
        names = golden_test_names()
        self.assertNotIn("rhi_null", names)
        self.assertNotIn("batched2d_vulkan_smoke", names)
        self.assertIn("harness_frame_capture", names)

    def test_text_bearing_goldens_keep_their_budget(self) -> None:
        document = golden_tolerances.load(TOLERANCE_FILE)
        for name in ("batched2d_overlay_golden", "batched2d_text_golden", "harness_frame_capture"):
            self.assertEqual(golden_tolerances.resolve(document, name), (1, 0.001), name)
        for name in ("batched2d_golden", "batched2d_cpu_golden", "cpu_wide_golden"):
            self.assertEqual(golden_tolerances.resolve(document, name), (0, 0.0), name)

    def test_cli_text_output_is_sorted_and_parses(self) -> None:
        document = golden_tolerances.load(TOLERANCE_FILE)
        result = run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, golden_tolerances.format_text(document))
        lines = result.stdout.splitlines()
        self.assertTrue(lines)
        names = []
        for line in lines:
            fields = line.split("\t")
            self.assertEqual(len(fields), 3, line)
            names.append(fields[0])
            self.assertGreaterEqual(int(fields[1]), 0)
            self.assertGreaterEqual(float(fields[2]), 0.0)
        self.assertEqual(names, sorted(names))

    def test_cli_json_matches_text_output(self) -> None:
        document = golden_tolerances.load(TOLERANCE_FILE)
        text_result = run_script()
        json_result = run_script("--json")
        self.assertEqual(json_result.returncode, 0, json_result.stderr)
        payload = json.loads(json_result.stdout)
        self.assertEqual(payload["schema"], 1)
        self.assertEqual(payload["defaults"], document["defaults"])
        rows = [
            (entry["name"], entry["tolerance"], entry["max_mismatch"])
            for entry in payload["tests"]
        ]
        self.assertEqual(rows, golden_tolerances.entries(document))
        self.assertEqual(json_result.stdout, run_script("--json").stdout)
        self.assertEqual(text_result.stdout, run_script().stdout)

    def test_cli_name_and_field(self) -> None:
        self.assertEqual(
            run_script("--name", "batched2d_overlay_golden", "--field", "tolerance").stdout,
            "1\n",
        )
        self.assertEqual(
            run_script("--name", "batched2d_overlay_golden", "--field", "max_mismatch").stdout,
            "0.001\n",
        )
        self.assertEqual(
            run_script("--name", "batched2d_golden").stdout,
            "batched2d_golden\t0\t0\n",
        )
        self.assertEqual(
            run_script("--name", "not_a_golden_test").stdout,
            "not_a_golden_test\t0\t0\n",
        )

    def test_cli_rejects_misuse(self) -> None:
        self.assertEqual(run_script("--field", "tolerance").returncode, 2)
        self.assertEqual(run_script("--json", "--name", "batched2d_golden").returncode, 2)

    def test_unknown_name_falls_back_to_defaults(self) -> None:
        document = golden_tolerances.load(TOLERANCE_FILE)
        self.assertEqual(
            golden_tolerances.resolve(document, "unknown_golden"),
            (document["defaults"]["tolerance"], document["defaults"]["max_mismatch"]),
        )

    def test_invalid_documents_are_rejected(self) -> None:
        base = json.loads(TOLERANCE_FILE.read_text(encoding="utf-8"))
        cases = {
            "wrong schema": {**base, "schema": 2},
            "missing defaults": {key: value for key, value in base.items() if key != "defaults"},
            "unknown top-level key": {**base, "extra": 1},
            "negative tolerance": {
                **base,
                "tests": {"x": {"tolerance": -1, "max_mismatch": 0.0}},
            },
            "boolean tolerance": {
                **base,
                "tests": {"x": {"tolerance": True, "max_mismatch": 0.0}},
            },
            "mismatch above one": {
                **base,
                "tests": {"x": {"tolerance": 0, "max_mismatch": 2.0}},
            },
            "unknown entry key": {
                **base,
                "tests": {"x": {"tolerance": 0, "max_mismatch": 0.0, "pixels": 1}},
            },
            "name with whitespace": {
                **base,
                "tests": {"bad name": {"tolerance": 0, "max_mismatch": 0.0}},
            },
            "missing entry field": {**base, "tests": {"x": {"tolerance": 0}}},
        }
        with tempfile.TemporaryDirectory() as directory:
            for label, document in cases.items():
                path = Path(directory) / f"{label.replace(' ', '_')}.json"
                path.write_text(json.dumps(document), encoding="utf-8")
                result = run_script(path=path)
                self.assertEqual(result.returncode, 1, f"{label}: {result.stdout}{result.stderr}")
                self.assertIn("golden_tolerances:", result.stderr, label)
                with self.assertRaises(golden_tolerances.ToleranceError, msg=label):
                    golden_tolerances.load(path)

            missing = run_script(path=Path(directory) / "missing.json")
            self.assertEqual(missing.returncode, 1, missing.stdout + missing.stderr)
            self.assertIn("golden_tolerances:", missing.stderr)

    def test_malformed_json_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "broken.json"
            path.write_text("{not json", encoding="utf-8")
            result = run_script(path=path)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("invalid JSON", result.stderr)


if __name__ == "__main__":
    unittest.main()
