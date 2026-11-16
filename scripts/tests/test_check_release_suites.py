#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture tests for scripts/check_release_suites.py.

Two halves, both about catching a lane that silently stops proving something:

* ``--workflows``: a scratch repository with a workflow whose suite step drops a required suite, or
  loses an ``--allow-skip`` reason, must fail; an in-sync pair must pass. This is the half the
  Lead's steer asked for - the lane's claim drifts from the matrix and CI notices without a run.
* ``--lane``: a scratch JUnit report whose required suite is skipped, absent, or whose output
  carries the validation-unavailable warning must fail.

No compiler, no GPU, no ctest: the reports and workflows are synthetic.

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
CHECK = SCRIPTS_DIR / "check_release_suites.py"

SUMMARY = "100% tests passed, 0 tests failed out of 3\n"

# Every suite LANE_MATRIX requires of each lane, so a runtime test can present a complete report.
LINUX_REQUIRED = [
    "batched2d_vulkan_validation",
    "batched2d_on_demand",
    "batched2d_golden",
    "batched2d_overlay_golden",
    "batched2d_text_golden",
    "batched2d_shapes_golden",
    "batched2d_cpu_golden",
    "batched2d_cpu_text_golden",
    "batched2d_cpu_vector_golden",
    "batched2d_cpu_image_golden",
    "cpu_wide_golden",
    "gallery_golden_light_none",
    "gallery_golden_high_contrast_focus",
    "gallery_golden_safe_area_fold_dark",
    "harness_frame_capture",
]

WINDOWS_REQUIRED = [
    "batched2d_cpu_image_golden",
    "cpu_wide_golden",
    "theme_golden",
    "harness_golden",
    "lowering_golden",
    "schema_golden",
    "codegen_vector_paint_golden",
]

WINDOWS_SKIP = [
    "batched2d_vulkan_validation",
    "batched2d_golden",
    "batched2d_overlay_golden",
    "batched2d_text_golden",
    "batched2d_shapes_golden",
    "batched2d_cpu_golden",
    "batched2d_cpu_text_golden",
    "batched2d_cpu_vector_golden",
    "harness_frame_capture",
]


def linux_cases(**overrides: object) -> list:
    """A complete Linux report; `overrides` maps a suite name to (skip, out)."""
    cases = []
    for name in LINUX_REQUIRED:
        case = overrides.get(name, (None,))
        cases.append((name,) + tuple(case))
    return cases


def windows_cases() -> list:
    cases = [(name, None) for name in WINDOWS_REQUIRED]
    cases += [(name, "SKIP_REGULAR_EXPRESSION_MATCHED") for name in WINDOWS_SKIP]
    return cases

LINUX_WORKFLOW = """\
name: Linux
jobs:
  linux:
    steps:
      - name: Tests
        run: ctest --preset ci
      - name: Release suites really ran (validation layer + goldens)
        run: |
          python3 -B scripts/check_release_suites.py \\
            --lane linux \\
            --require batched2d_golden \\
            --require batched2d_vulkan_validation \\
            --forbid-text "validation layer not available"
"""

WINDOWS_WORKFLOW = """\
name: Windows
jobs:
  windows:
    steps:
      - name: Release suites really ran (GPU-free goldens)
        shell: pwsh
        run: |
          python -B scripts/check_release_suites.py `
            --lane windows `
            --require cpu_wide_golden `
            --allow-skip "batched2d_golden=lavapipe-only"
"""


def junit(cases: list) -> str:
    """cases: list of (name, skip_message_or_None, system_out_or_None)."""
    body = []
    for case in cases:
        name, skip = case[0], case[1]
        out = case[2] if len(case) > 2 else None
        inner = ""
        if skip is not None:
            inner += '<skipped message="{}"/>'.format(skip)
        inner += "<properties/>"
        if out is not None:
            inner += "<system-out>{}</system-out>".format(out)
        status = "run" if skip is None else "notrun"
        body.append(
            '<testcase name="{}" classname="{}" status="{}">{}</testcase>'.format(
                name, name, status, inner
            )
        )
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<testsuite name="(empty)" tests="{}">{}</testsuite>'.format(len(cases), "".join(body))
    )


class RuntimeGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def run_gate(self, cases: list, log: str = SUMMARY, lane: str = "linux") -> tuple:
        report = self.root / "junit.xml"
        report.write_text(junit(cases), encoding="utf-8")
        log_path = self.root / "run.log"
        log_path.write_text(log, encoding="utf-8")
        result = subprocess.run(
            [sys.executable, "-B", str(CHECK), "--lane", lane,
             "--junit", str(report), "--log", str(log_path)],
            capture_output=True, text=True, encoding="utf-8",
        )
        return result.returncode, result.stdout, result.stderr

    def test_all_required_ran(self) -> None:
        code, out, err = self.run_gate(linux_cases())
        self.assertEqual(code, 0, out + err)
        payload = json.loads(out[: out.rindex("}") + 1])
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["ran"], len(LINUX_REQUIRED))

    def test_skipped_required_suite_fails(self) -> None:
        code, out, err = self.run_gate(
            linux_cases(batched2d_golden=("SKIP_REGULAR_EXPRESSION_MATCHED",))
        )
        self.assertEqual(code, 1, out)
        self.assertIn("batched2d_golden", err)

    def test_declared_skip_passes_and_is_reported(self) -> None:
        # The Windows lane: required suites run, declared-unable ones skip with a reason.
        code, out, err = self.run_gate(windows_cases(), lane="windows")
        self.assertEqual(code, 0, err)
        self.assertIn("lavapipe-only", out)

    def test_declared_skip_that_runs_is_noted(self) -> None:
        cases = [(name, None) for name in WINDOWS_REQUIRED]
        cases += [(name, None) for name in WINDOWS_SKIP]  # all ran instead of skipping
        code, out, err = self.run_gate(cases, lane="windows")
        self.assertEqual(code, 0, err)
        self.assertIn("drop the exemption", out)

    def test_missing_suite_fails(self) -> None:
        cases = [c for c in linux_cases() if c[0] != "batched2d_golden"]
        code, out, err = self.run_gate(cases)
        self.assertEqual(code, 1, out)
        self.assertIn("not in the report", err)

    def test_forbidden_validation_warning_fails(self) -> None:
        log = SUMMARY + "[vulkan] warning: validation layer not available\n"
        code, out, err = self.run_gate(linux_cases(), log=log)
        self.assertEqual(code, 1, out)
        self.assertIn("validation layer not available", err)

    def test_forbidden_text_found_in_junit_output_only(self) -> None:
        # --output-on-failure hides a passing test's stdout from the log, so the warning can live
        # only in the JUnit <system-out>. The gate must still catch it.
        code, out, err = self.run_gate(
            linux_cases(
                batched2d_vulkan_validation=(
                    None,
                    "[vulkan] warning: validation layer not available",
                )
            )
        )
        self.assertEqual(code, 1, out)
        self.assertIn("validation layer not available", err)

    def test_utf16_log_is_read(self) -> None:
        # Windows runners write Tee-Object logs as UTF-16LE; a UTF-8 read would find no summary
        # line and fail every lane for the wrong reason.
        report = self.root / "junit.xml"
        report.write_text(junit(linux_cases()), encoding="utf-8")
        log = self.root / "run.log"
        log.write_text(SUMMARY, encoding="utf-16")
        result = subprocess.run(
            [sys.executable, "-B", str(CHECK), "--lane", "linux",
             "--junit", str(report), "--log", str(log)],
            capture_output=True, text=True, encoding="utf-8",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_empty_report_is_setup_error(self) -> None:
        code, _, err = self.run_gate([])
        self.assertEqual(code, 2)
        self.assertIn("no test cases", err)

    def test_log_without_summary_fails(self) -> None:
        code, out, err = self.run_gate(linux_cases(), log="no summary here\n")
        self.assertEqual(code, 1, out)
        self.assertIn("no summary line", err)

    def test_unknown_lane_is_setup_error(self) -> None:
        code, _, err = self.run_gate([("a", None)], lane="plan9")
        self.assertEqual(code, 2)
        self.assertIn("unknown lane", err)

    def test_missing_junit_is_setup_error(self) -> None:
        result = subprocess.run(
            [sys.executable, "-B", str(CHECK), "--lane", "linux",
             "--junit", str(self.root / "nope.xml")],
            capture_output=True, text=True, encoding="utf-8",
        )
        self.assertEqual(result.returncode, 2)


class WorkflowMatrixTests(unittest.TestCase):
    """The static half: a lane must keep declaring every suite the matrix says it runs.

    The real LANE_MATRIX is replaced in the scratch copy with a two-entry matrix matching
    LINUX_WORKFLOW/WINDOWS_WORKFLOW above, so these tests assert the drift *mechanism* rather than
    coupling to the repository's current suite list (which would make every matrix edit a test
    edit).
    """

    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        (self.root / ".github" / "workflows").mkdir(parents=True)
        (self.root / "scripts").mkdir()
        source = CHECK.read_text(encoding="utf-8")
        # Swap the real matrix for the fixture one: the marker is the LANE_MATRIX block's opening.
        head, _, rest = source.partition("LANE_MATRIX: Dict[str, Dict[str, object]] = {")
        _, _, tail = rest.partition("\n# The CTest summary line")
        fixture_matrix = (
            'LANE_MATRIX: Dict[str, Dict[str, object]] = {\n'
            '    "linux": {\n'
            '        "workflow": ".github/workflows/linux.yml",\n'
            '        "required": ["batched2d_golden", "batched2d_vulkan_validation"],\n'
            '        "allow_skip": {},\n'
            '        "forbid_text": ["validation layer not available"],\n'
            '    },\n'
            '    "windows": {\n'
            '        "workflow": ".github/workflows/windows.yml",\n'
            '        "required": ["cpu_wide_golden"],\n'
            '        "allow_skip": {"batched2d_golden": "lavapipe-only"},\n'
            '        "forbid_text": [],\n'
            '    },\n'
            '}\n'
        )
        self.check = self.root / "scripts" / "check_release_suites.py"
        self.check.write_text(head + fixture_matrix + "\n# The CTest summary line" + tail,
                              encoding="utf-8")

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write_workflows(self, linux: str, windows: str) -> None:
        workflows = self.root / ".github" / "workflows"
        (workflows / "linux.yml").write_text(linux, encoding="utf-8")
        (workflows / "windows.yml").write_text(windows, encoding="utf-8")

    def run_check(self) -> tuple:
        result = subprocess.run(
            [sys.executable, "-B", str(self.check), "--workflows"],
            capture_output=True, text=True, encoding="utf-8",
            cwd=str(self.root),
        )
        return result.returncode, result.stdout, result.stderr

    def test_matching_workflows_pass(self) -> None:
        self.write_workflows(LINUX_WORKFLOW, WINDOWS_WORKFLOW)
        code, out, err = self.run_check()
        self.assertEqual(code, 0, out + err)

    def test_dropped_required_suite_fails(self) -> None:
        # The Linux lane stops requiring batched2d_vulkan_validation: exactly the drift the check
        # exists to catch.
        drifted = LINUX_WORKFLOW.replace(
            "            --require batched2d_vulkan_validation \\\n", ""
        )
        self.write_workflows(drifted, WINDOWS_WORKFLOW)
        code, out, err = self.run_check()
        self.assertEqual(code, 1, out)
        self.assertIn("no longer requires", err)
        self.assertIn("batched2d_vulkan_validation", err)

    def test_lost_allow_skip_reason_fails(self) -> None:
        drifted = WINDOWS_WORKFLOW.replace(
            '            --allow-skip "batched2d_golden=lavapipe-only"\n', ""
        )
        self.write_workflows(LINUX_WORKFLOW, drifted)
        code, out, err = self.run_check()
        self.assertEqual(code, 1, out)
        self.assertIn("lost its --allow-skip reason", err)

    def test_unknown_required_name_fails(self) -> None:
        # A workflow that requires something the matrix does not know is drift too: the lane would
        # be claiming a suite nobody reasons about.
        drifted = WINDOWS_WORKFLOW.replace(
            "            --require cpu_wide_golden `\n",
            "            --require cpu_wide_golden `\n            --require mystery_suite `\n",
        )
        self.write_workflows(LINUX_WORKFLOW, drifted)
        code, out, err = self.run_check()
        self.assertEqual(code, 1, out)
        self.assertIn("not in LANE_MATRIX", err)

    def test_missing_step_fails(self) -> None:
        self.write_workflows(
            "name: Linux\njobs:\n  linux:\n    steps:\n      - name: Tests\n        run: ctest\n",
            WINDOWS_WORKFLOW,
        )
        code, out, err = self.run_check()
        self.assertEqual(code, 1, out)
        self.assertIn("stopped declaring its suites", err)

    def test_missing_workflow_fails(self) -> None:
        workflows = self.root / ".github" / "workflows"
        (workflows / "linux.yml").write_text(LINUX_WORKFLOW, encoding="utf-8")
        code, out, err = self.run_check()
        self.assertEqual(code, 1, out)
        self.assertIn("workflow file is missing", err)


if __name__ == "__main__":
    unittest.main()
