#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Release-suite gate for issue #199: enforce the lane/suite matrix, then verify a run honoured it.

The acceptance is "release builds on all lanes run validation-layer and golden suites and publish
artifacts". "All lanes" cannot be literally true - a hosted Windows runner has no lavapipe ICD and
the Windows job builds with ``TESSERA_BUILD_VULKAN=OFF``, so Windows cannot run the lavapipe goldens
or the validation layer at all. The honest deliverable is therefore a *matrix* (which suite runs on
which lane, and why each cell that cannot run cannot) plus this gate, which fails when a lane
silently stops declaring a suite. Rationale and the full table: docs/guides/packaging.md.

``--workflows``  Static. Parses the workflow YAML as text (stdlib only, no PyYAML) and asserts each
                 lane's suite step declares exactly the LANE_MATRIX set. This is the mode that
                 catches "the lane quietly dropped a suite": a runtime report cannot, because a
                 suite that is never declared never appears in the JUnit file.
``--lane``       Runtime. Reads the CTest ``--output-junit`` report plus the run log and asserts
                 every required suite exists and *ran* rather than skipped. Necessary because
                 ``batched2d_vulkan_validation`` warns and then passes without the layer, the
                 goldens carry ``SKIP_REGULAR_EXPRESSION``, and CTest exits 0 on an empty filter.

Exit codes: 0 every requirement held, 1 a requirement failed, 2 setup/usage failure.

  python -B scripts/check_release_suites.py --workflows
  python -B scripts/check_release_suites.py --lane linux --junit J --log L
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Dict, List, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parent.parent

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_SETUP = 2

# The lane/suite matrix: the single source of truth this gate enforces. Each workflow's suite step
# must declare exactly these names, with the reason stated for every suite the lane cannot run.
#
# Reasoning for the split (see docs/guides/packaging.md, "Release CI lanes"):
#   linux    has lavapipe (the ADR-009 reference renderer) and the Khronos validation layer.
#   windows  is a hosted runner: no GPU, no lavapipe package, and the job configures
#            TESSERA_BUILD_VULKAN=OFF, so no Vulkan instance exists to validate.
LANE_MATRIX: Dict[str, Dict[str, object]] = {
    "linux": {
        "workflow": ".github/workflows/linux.yml",
        "required": [
            # Validation-layer suites.
            "batched2d_vulkan_validation",
            "batched2d_on_demand",
            # Lavapipe (ADR-009) image goldens.
            "batched2d_golden",
            "batched2d_overlay_golden",
            "batched2d_text_golden",
            "batched2d_shapes_golden",
            # CPU goldens (bit-exact everywhere).
            "batched2d_cpu_golden",
            "batched2d_cpu_text_golden",
            "batched2d_cpu_vector_golden",
            "batched2d_cpu_image_golden",
            "cpu_wide_golden",
            # Lavapipe-rendered gallery + harness goldens.
            "gallery_golden_light_none",
            "gallery_golden_high_contrast_focus",
            "gallery_golden_safe_area_fold_dark",
            "harness_frame_capture",
        ],
        "allow_skip": {},
        "forbid_text": ["validation layer not available"],
    },
    "windows": {
        "workflow": ".github/workflows/windows.yml",
        "required": [
            # The GPU-free golden corpus: bit-exact on both hosts, so a real cross-platform check.
            "batched2d_cpu_image_golden",
            "cpu_wide_golden",
            "theme_golden",
            "harness_golden",
            "lowering_golden",
            "schema_golden",
        ],
        "allow_skip": {
            "batched2d_vulkan_validation": "no Vulkan device: this lane configures TESSERA_BUILD_VULKAN=OFF",
            "batched2d_golden": "lavapipe-only: registered under 'if (UNIX AND TARGET batched2d_poc)'",
            "batched2d_overlay_golden": "lavapipe-only, same registration as batched2d_golden",
            "batched2d_text_golden": "lavapipe-only, same registration as batched2d_golden",
            "batched2d_shapes_golden": "lavapipe-only, same registration as batched2d_golden",
            "batched2d_cpu_golden": "needs batched2d_poc, which only exists with TESSERA_BUILD_VULKAN=ON",
            "batched2d_cpu_text_golden": "needs batched2d_poc, same reason as batched2d_cpu_golden",
            "batched2d_cpu_vector_golden": "ThorVG off on this lane",
            "codegen_vector_paint_golden": "ThorVG off on this lane: the entry is gated on TESSERA_ENABLE_THORVG",
            "harness_frame_capture": "needs the Vulkan harness capture path",
        },
        "forbid_text": [],
    },
}

# The CTest summary line; its absence means the run did not complete.
# CTest prints the failure clause only when something failed: "100% tests passed, 0 tests
# failed out of 595" versus "100% tests passed out of 595". Both mean the run completed, and
# matching only the first form made a fully green release lane report "no summary line".
SUMMARY_RE = re.compile(r"\d+% tests passed(?:, \d+ tests? failed)? out of \d+")
REQUIRE_RE = re.compile(r"--require[\s\"']+([A-Za-z0-9_]+)")
ALLOW_SKIP_RE = re.compile(r"--allow-skip[\s\"']+([A-Za-z0-9_]+)\s*=")
# The step that declares the suites, so the static check compares the right block rather than the
# whole file (a lane may mention a suite in a comment).
SUITE_STEP_RE = re.compile(r"name:\s*Release.*suites.*", re.IGNORECASE)


def read_text(path: Path) -> str:
    """Read a file that may be UTF-8 or UTF-16.

    Windows runners write ``Tee-Object``/``Out-File`` logs as UTF-16LE by default, and a UTF-8 read
    of that yields NUL bytes between every character, which would make every textual assertion
    silently pass or silently fail. A BOM decides when present; otherwise UTF-8, falling back to
    UTF-16.
    """
    data = path.read_bytes()
    if data.startswith(b"\xff\xfe") or data.startswith(b"\xfe\xff"):
        return data.decode("utf-16", errors="replace")
    if data.startswith(b"\xef\xbb\xbf"):
        return data.decode("utf-8-sig", errors="replace")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("utf-16", errors="replace")


def emit(gate: str, problems: List[str], notes: List[str], extra: Optional[Dict] = None,
         ok_message: str = "") -> int:
    """Print the JSON report, the notes, and any failures; return the process exit code."""
    payload: Dict = {"ok": not problems, "gate": gate, "problems": problems, "notes": notes}
    payload.update(extra or {})
    print(json.dumps(payload, indent=2, sort_keys=True))
    for note in notes:
        print("note: {}".format(note))
    if problems:
        for problem in problems:
            print("FAIL: {}".format(problem), file=sys.stderr)
        return EXIT_FAILED
    print(ok_message)
    return EXIT_OK


def suite_step_text(workflow: Path) -> str:
    """The `run:` block of the step that declares suites, plus its following `--allow-skip` lines.

    A textual slice rather than a YAML parse: PyYAML is not guaranteed on a hosted runner and every
    other gate in scripts/ is stdlib-only.
    """
    lines = read_text(workflow).splitlines()
    start = None
    for index, line in enumerate(lines):
        if SUITE_STEP_RE.search(line):
            start = index
            break
    if start is None:
        return ""
    collected: List[str] = []
    for line in lines[start:]:
        stripped = line.strip()
        # A new step or job ends the block.
        if collected and (stripped.startswith("- name:") or stripped.startswith("- uses:")
                          or re.match(r"^  [a-z0-9_-]+:$", line)):
            break
        collected.append(line)
    return "\n".join(collected)


def check_workflows() -> int:
    problems: List[str] = []
    notes: List[str] = []
    for lane, spec in sorted(LANE_MATRIX.items()):
        workflow = REPO_ROOT / str(spec["workflow"])
        if not workflow.is_file():
            problems.append("{}: workflow file is missing: {}".format(lane, workflow))
            continue
        block = suite_step_text(workflow)
        if not block:
            problems.append(
                "{}: no step named 'Release ... suites' in {} - the lane stopped declaring its "
                "suites".format(lane, spec["workflow"])
            )
            continue
        declared_required = set(REQUIRE_RE.findall(block))
        declared_skip = set(ALLOW_SKIP_RE.findall(block))
        expected_required = set(spec["required"])  # type: ignore[arg-type]
        expected_skip = set(spec["allow_skip"])  # type: ignore[arg-type]

        missing = sorted(expected_required - declared_required)
        if missing:
            problems.append(
                "{}: the suite step no longer requires: {}".format(lane, ", ".join(missing))
            )
        dropped = sorted(expected_skip - declared_skip)
        if dropped:
            problems.append(
                "{}: a declared inability to run lost its --allow-skip reason: {}".format(
                    lane, ", ".join(dropped)
                )
            )
        # The matrix is the source of truth in both directions: a name that appears only in the
        # workflow is drift that silently widened what the lane claims or excused.
        extra_required = sorted(declared_required - expected_required)
        if extra_required:
            problems.append(
                "{}: the suite step requires names not in LANE_MATRIX (add them to the matrix): "
                "{}".format(lane, ", ".join(extra_required))
            )
        extra_skip = sorted(declared_skip - expected_skip)
        if extra_skip:
            problems.append(
                "{}: the suite step allows skips not in LANE_MATRIX (add them to the matrix): "
                "{}".format(lane, ", ".join(extra_skip))
            )
        notes.append(
            "{}: {} required, {} declared unable to run".format(
                lane, len(expected_required), len(expected_skip)
            )
        )

    return emit(
        "release-suites-workflows",
        problems,
        notes,
        extra={"lanes": sorted(LANE_MATRIX)},
        ok_message="release suite matrix ok: {} lane(s)".format(len(LANE_MATRIX)),
    )


class Report:
    """Collected JUnit cases plus every captured output stream.

    The output streams matter as much as the pass/skip status: CTest run with
    ``--output-on-failure`` suppresses a *passing* test's stdout, so a warning printed by a green
    test never reaches the log. The JUnit report does carry it in ``<system-out>``.
    """

    def __init__(self) -> None:
        self.cases: Dict[str, Optional[str]] = {}
        self.text: List[str] = []

    def add(self, name: str, skip_message: Optional[str]) -> None:
        self.cases[name] = skip_message

    def ran(self, name: str) -> bool:
        return name in self.cases and self.cases[name] is None

    @property
    def ran_count(self) -> int:
        return sum(1 for message in self.cases.values() if message is None)


def load_junit(path: Path) -> Report:
    root = ElementTree.fromstring(read_text(path))
    # A bare <testsuite> and a <testsuites> wrapper both occur, and CTest may emit several suites.
    suites = [root] if root.tag == "testsuite" else list(root.iter("testsuite"))
    report = Report()
    for suite in suites:
        for case in suite.iter("testcase"):
            name = case.get("name") or ""
            if not name:
                continue
            skipped = case.find("skipped")
            report.add(name, None if skipped is None else (skipped.get("message") or "skipped"))
            for tag in ("system-out", "system-err", "failure", "error"):
                for element in case.iter(tag):
                    if element.text:
                        report.text.append(element.text)
    return report


def check_lane(args: argparse.Namespace) -> int:
    problems: List[str] = []
    notes: List[str] = []
    spec = LANE_MATRIX.get(args.lane)
    if spec is None:
        print("error: unknown lane {!r}; known: {}".format(args.lane, ", ".join(sorted(LANE_MATRIX))),
              file=sys.stderr)
        return EXIT_SETUP

    try:
        report = load_junit(Path(args.junit))
    except (OSError, ElementTree.ParseError) as error:
        print("error: cannot read JUnit report {}: {}".format(args.junit, error), file=sys.stderr)
        return EXIT_SETUP
    if not report.cases:
        print("error: the JUnit report lists no test cases", file=sys.stderr)
        return EXIT_SETUP

    log = ""
    if args.log:
        try:
            log = read_text(Path(args.log))
        except OSError as error:
            print("error: cannot read log {}: {}".format(args.log, error), file=sys.stderr)
            return EXIT_SETUP

    allowed: Dict[str, str] = dict(spec["allow_skip"])  # type: ignore[arg-type]

    # The declaration the workflow passed must agree with LANE_MATRIX, and vice versa. A missing
    # requirement is a lane that stopped asking for a suite; an extra one is a suite the matrix does
    # not know about. Either is drift, so both are reported.
    declared_require = list(args.require or [])
    if declared_require:
        missing = [n for n in spec["required"] if n not in declared_require]  # type: ignore[attr-defined]
        extra = [n for n in declared_require if n not in spec["required"]]  # type: ignore[attr-defined]
        if missing:
            problems.append("the invocation does not require: {}".format(", ".join(missing)))
        if extra:
            problems.append("the invocation requires suites LANE_MATRIX does not: {}".format(
                ", ".join(extra)))

    declared_skip: Dict[str, str] = {}
    for pair in args.allow_skip or []:
        name, _, reason = pair.partition("=")
        declared_skip[name.strip()] = reason.strip()
    # One direction only: the invocation may restate fewer skips than the matrix declares (a lane
    # that does not pass --allow-skip at all is opting into the matrix wholesale), but it must not
    # ADD a skip the matrix does not know about, and a reason it does state must not contradict the
    # matrix -- that is the drift this cross-check exists for.
    only_declared = sorted(set(declared_skip) - set(allowed))
    if only_declared:
        problems.append("the invocation allows skips LANE_MATRIX does not declare: {}".format(
            ", ".join(only_declared)))
    for name in sorted(set(declared_skip) & set(allowed)):
        if declared_skip[name] != allowed[name]:
            # Wording, not substance: both sides agree the suite cannot run here. Report it so a
            # reader can align them, but do not fail a release lane over prose.
            notes.append("'{}': the invocation's skip reason is worded differently from "
                         "LANE_MATRIX".format(name))

    # --forbid-text must include the matrix's list; the log check below then uses the union.
    forbidden_text = sorted(set(spec.get("forbid_text", [])) | set(args.forbid_text or []))  # type: ignore[arg-type]

    if log and SUMMARY_RE.search(log) is None:
        problems.append("the CTest log has no summary line, so the run did not complete")

    for name in spec["required"]:  # type: ignore[attr-defined]
        if name not in report.cases:
            problems.append("required suite '{}' is not in the report".format(name))
        elif not report.ran(name):
            reason = allowed.get(name)
            if reason is None:
                problems.append("required suite '{}' skipped ({})".format(name, report.cases[name]))
            else:
                notes.append("'{}' skipped as declared: {}".format(name, reason))

    # A suite declared unable to run on this lane should actually skip, and the run report should
    # restate the reason: that is the written record of the gap. If it ran instead, the declaration
    # is stale and should say so rather than quietly keep the exemption.
    for name, reason in sorted(allowed.items()):
        if report.ran(name):
            notes.append("'{}' was declared unable to run but ran; drop the exemption".format(name))
        elif name in report.cases:
            notes.append("'{}' skipped as declared: {}".format(name, reason))
        else:
            notes.append("'{}' is declared unable to run and is not registered".format(name))

    for text in forbidden_text:
        # Scan the log AND the JUnit output streams: --output-on-failure hides a passing test's
        # stdout from the log, so a layer-unavailable warning can survive only in the JUnit file.
        if (log and text in log) or any(text in chunk for chunk in report.text):
            problems.append("the output contains forbidden text: {!r}".format(text))

    ran = report.ran_count
    return emit(
        "release-suites",
        problems,
        notes,
        extra={
            "lane": args.lane,
            "cases": len(report.cases),
            "ran": ran,
            "skipped": len(report.cases) - ran,
        },
        ok_message="release suites ok ({}): {} case(s), {} ran, {} skipped".format(
            args.lane, len(report.cases), ran, len(report.cases) - ran
        ),
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workflows", action="store_true",
                        help="statically enforce the lane/suite matrix in the workflow YAML")
    parser.add_argument("--junit", default=None, help="CTest --output-junit report")
    parser.add_argument("--log", default=None, help="the CTest run log")
    parser.add_argument("--lane", default=None, help="lane to verify against LANE_MATRIX")
    # The build jobs pass their suite list explicitly, which is the declaration `--workflows` checks
    # against LANE_MATRIX. Accept it here too, and verify it agrees: accepting without verifying
    # would let the workflow and the matrix diverge silently, and ignoring the flags (the previous
    # behaviour) made every release lane fail with a usage error.
    parser.add_argument("--require", action="append", default=[], metavar="SUITE",
                        help="a suite this lane must have run (repeatable; cross-checked with LANE_MATRIX)")
    parser.add_argument("--allow-skip", action="append", default=[], metavar="SUITE=REASON",
                        help="a suite this lane cannot run, with the reason (repeatable)")
    parser.add_argument("--forbid-text", action="append", default=[], metavar="TEXT",
                        help="text that must not appear in the run log (repeatable)")
    args = parser.parse_args(argv)
    if args.workflows:
        return check_workflows()
    if not args.lane or not args.junit:
        print("error: pass --workflows, or --lane and --junit", file=sys.stderr)
        return EXIT_SETUP
    return check_lane(args)


if __name__ == "__main__":
    sys.exit(main())
