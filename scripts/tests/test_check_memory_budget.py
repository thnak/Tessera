#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CLI self-tests for scripts/check_memory_budget.py (the issue #13 two-sided ratchet).

    python -B -m unittest discover -s scripts/tests -p "test_check_memory*" -v

Every test builds a throwaway tree with a stand-in probe and a budget file and runs the real CLI, so
the cases are about what a user sees -- the exit code, the words on stdout/stderr, and whether the
budget file changed -- rather than about internals.

The stand-in probe is a Python script that emits `name=value` lines, so a test can make any row
move without building C++. That is the same shape the real probe prints
(`tests/benchmarks/memory/memory_sizes_probe.cpp`), and `TestRealProbe` checks that shape against the
committed budget when a build tree happens to exist, so the fixture cannot silently drift from what
the gate actually consumes.

The cases are the behaviours the three-status split buys, plus the ones that keep it honest:

* a deterministic row that moves past its budget -> exit 1 (the regression side),
* a deterministic row much better than its budget -> exit 1 telling you to tighten (staleness),
* every exact row inside a 0% band -> exit 0,
* `--update` re-pins a tightening but refuses to loosen, and writes nothing when it refuses,
* a `warn` row reports the same regression as a warning, not an error,
* an `unmeasured` row needs a reason and must not carry a budget (no invented numbers),
* a warning-only budget file is a pass: the gate never fails on a sampled row,
* the deterministic rows are exactly reproducible: two runs must agree, and a probe that disagrees
  about its own "exact" row is an error rather than a coin flip,
* a malformed budget, an unknown status or a missing probe is not a pass.
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
GATE = SCRIPTS_DIR / "check_memory_budget.py"
RATCHET = SCRIPTS_DIR / "memory_budget_ratchet.py"
MEASURE = SCRIPTS_DIR / "memory_budget_measure.py"

sys.path.insert(0, str(SCRIPTS_DIR))
import memory_budget_measure as measure  # noqa: E402  (sibling script, stdlib only)
import memory_budget_ratchet as ratchet  # noqa: E402  (sibling script, stdlib only)

# The rows a stand-in probe must print. Taken from the measurement module so a test cannot drift from
# what the gate actually reads.
EXACT_ROWS = measure.DETERMINISTIC_ROWS
SAMPLED_ROWS = measure.SAMPLED_ROWS

# A value for every row: small distinct integers, so a test can say "row X is N" without arithmetic.
BASE_VALUES = {name: 100 for name in EXACT_ROWS + SAMPLED_ROWS}

# The configuration the stand-in probe reports, and the one the budget fixture pins.
CONFIG = "Release"


def probe_script(counter_path: Path, values: dict, drift: dict | None = None, config: str = CONFIG) -> str:
    """A stand-in probe. `drift` overrides a row on the second invocation only.

    The drift case is what the gate's reproducibility check exists for: a "deterministic" row that
    takes two different values in one measurement must not silently become whichever sample ran last.
    `counter_path` must be per-test-case; a shared counter would make the "first run" depend on a
    previous test and turn the drift case into a coin flip.
    """
    drift = drift or {}
    lines = [
        "import sys",
        "from pathlib import Path",
        "p = Path({!r})".format(str(counter_path)),
        "n = int(p.read_text()) if p.is_file() else 0",
        "p.write_text(str(n + 1))",
        "values = {!r}".format(values),
        "drift = {!r}".format(drift),
        "if n > 0:",
        "    values = dict(values)",
        "    values.update(drift)",
        # The configuration comment the real probe prints. The gate refuses a measurement from an
        # unknown configuration, so the stand-in has to carry one too.
        "print('# config=' + {!r})".format(config),
        "for name in sorted(values):",
        "    print('{}={}'.format(name, values[name]))",
    ]
    return "\n".join(lines) + "\n"


def budget_document(values: dict, status_by_row: dict | None = None, tolerance: dict | None = None):
    """A minimal valid budget whose entries pin every row to `values`."""
    status_by_row = status_by_row or {}
    entries = {}
    for name in sorted(values):
        entry = {
            "budget": values[name],
            "status": status_by_row.get(name, "enforce"),
            "unit": "bytes",
            "measures": "test row {}".format(name),
        }
        entries[name] = entry
    entries["android_pss_bytes"] = {
        "status": "unmeasured",
        "unit": "bytes",
        "measures": "the Android PSS split",
        "reason": "no Android sample in this repository",
    }
    return {
        "schema": 1,
        "note": "test budget",
        "source": {"probe": "stand-in"},
        "measurement": {"build_config": CONFIG, "nodes": 1},
        "tolerance": tolerance or {"default": {"better": 0.0, "worse": 0.0}},
        "entries": entries,
    }


class GateTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name) / "repo"
        # The probe is found by globbing the build directory for `memory_sizes_probe`, so the
        # stand-in lives under a fake build tree at that name.
        self.build = self.root / "build" / "ci"
        self.probe = self.build / "tests" / "benchmarks" / "memory" / measure.PROBE_TARGET
        self.probe.parent.mkdir(parents=True)
        self.budget = self.root / "budget.json"

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write_probe(self, values: dict, drift: dict | None = None) -> None:
        # The counter lives in this case's temp tree, so each test starts at run zero.
        counter = self.root / "probe-run-count"
        self.probe.write_text(probe_script(counter, values, drift), encoding="utf-8")

    def write_budget(self, document: dict) -> None:
        self.budget.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")

    def run_gate(self, *extra: str, env_extra: dict | None = None):
        environment = dict(os.environ)
        # The stand-in probe is a Python script, which Windows will not exec directly (no shebang
        # execution), so the gate runs it through this interpreter. The real probe is a native
        # binary and needs no such help.
        environment.setdefault("TESSERA_MEMORY_PROBE_INTERPRETER", sys.executable)
        if env_extra:
            environment.update(env_extra)
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(GATE),
                "--budget",
                str(self.budget),
                "--build-dir",
                str(self.build),
                *extra,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=str(REPO_ROOT),
            env=environment,
        )

    def budget_text(self) -> str:
        return self.budget.read_text(encoding="utf-8")


class ExactRowsTests(GateTestCase):
    def test_all_exact_rows_at_their_budget_pass(self) -> None:
        self.write_probe(BASE_VALUES)
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 error(s)", result.stdout)

    def test_a_worse_exact_row_fails(self) -> None:
        worse = dict(BASE_VALUES)
        worse["sizeof_node"] = 132  # budget 100
        self.write_probe(worse)
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("sizeof_node", result.stdout)
        self.assertIn("ERROR", result.stdout)

    def test_a_better_exact_row_is_stale_and_fails(self) -> None:
        better = dict(BASE_VALUES)
        better["sizeof_node"] = 90  # well under the pinned 100
        self.write_probe(better)
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("much better than its budget", result.stdout)
        self.assertIn("--update", result.stdout)

    def test_a_deterministic_row_that_moves_between_runs_is_an_error(self) -> None:
        # The probe reports 100 first and 140 after: the row is not reproducible, so no verdict on
        # it can be trusted, whatever the budget says.
        self.write_probe(BASE_VALUES, drift={"sizeof_node": 140})
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not reproducible", result.stdout)

    def test_two_identical_runs_of_the_same_tree_agree(self) -> None:
        self.write_probe(BASE_VALUES)
        self.write_budget(budget_document(BASE_VALUES))
        first = self.run_gate("--check", "--json")
        second = self.run_gate("--check", "--json")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        # Byte-identical apart from nothing: the same tree must measure the same numbers.
        self.assertEqual(first.stdout, second.stdout)


class WarnRowTests(GateTestCase):
    def test_a_worse_warn_row_warns_and_still_passes(self) -> None:
        values = dict(BASE_VALUES)
        values["resident_bytes_per_node"] = 500  # budget 100, band +300% -> upper bound 400
        self.write_probe(values)
        self.write_budget(
            budget_document(
                BASE_VALUES,
                status_by_row={"resident_bytes_per_node": "warn"},
                tolerance={
                    "default": {"better": 0.0, "worse": 0.0},
                    "resident_bytes_per_node": {"better": 20.0, "worse": 300.0},
                },
            )
        )
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARN", result.stdout)
        self.assertIn("warn-only entry", result.stdout)

    def test_a_warn_row_inside_its_generous_band_is_quiet(self) -> None:
        values = dict(BASE_VALUES)
        values["resident_bytes_per_node"] = 250  # within 100 + 300%
        self.write_probe(values)
        self.write_budget(
            budget_document(
                BASE_VALUES,
                status_by_row={"resident_bytes_per_node": "warn"},
                tolerance={
                    "default": {"better": 0.0, "worse": 0.0},
                    "resident_bytes_per_node": {"better": 20.0, "worse": 300.0},
                },
            )
        )
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("WARN", result.stdout)


class UnmeasuredRowTests(GateTestCase):
    def test_an_unmeasured_row_is_reported_with_its_reason_and_does_not_fail(self) -> None:
        self.write_probe(BASE_VALUES)
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("android_pss_bytes", result.stdout)
        self.assertIn("declared, not measured", result.stdout)
        self.assertIn("no Android sample", result.stdout)

    def test_an_unmeasured_row_with_a_budget_is_rejected(self) -> None:
        document = budget_document(BASE_VALUES)
        document["entries"]["android_pss_bytes"]["budget"] = 200 * 1024 * 1024
        self.write_probe(BASE_VALUES)
        self.write_budget(document)
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("unmeasured", result.stderr)
        self.assertIn("invention", result.stderr)

    def test_an_unmeasured_row_without_a_reason_is_rejected(self) -> None:
        document = budget_document(BASE_VALUES)
        del document["entries"]["android_pss_bytes"]["reason"]
        self.write_probe(BASE_VALUES)
        self.write_budget(document)
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("no `reason`", result.stderr)


class UpdateTests(GateTestCase):
    def test_update_tightens_a_better_row(self) -> None:
        better = dict(BASE_VALUES)
        better["sizeof_node"] = 80
        self.write_probe(better)
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("tightened", result.stdout)
        written = json.loads(self.budget_text())
        self.assertEqual(written["entries"]["sizeof_node"]["budget"], 80)

    def test_update_refuses_to_loosen_and_writes_nothing(self) -> None:
        worse = dict(BASE_VALUES)
        worse["sizeof_node"] = 140
        self.write_probe(worse)
        self.write_budget(budget_document(BASE_VALUES))
        before = self.budget_text()
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("refuses to loosen", result.stderr)
        # Nothing was written: a refused update must not partially re-pin the other rows.
        self.assertEqual(self.budget_text(), before)

    def test_update_refuses_to_loosen_a_warn_row_too(self) -> None:
        values = dict(BASE_VALUES)
        values["resident_bytes_per_node"] = 500
        self.write_probe(values)
        document = budget_document(
            BASE_VALUES,
            status_by_row={"resident_bytes_per_node": "warn"},
            tolerance={
                "default": {"better": 0.0, "worse": 0.0},
                "resident_bytes_per_node": {"better": 20.0, "worse": 300.0},
            },
        )
        self.write_budget(document)
        before = self.budget_text()
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("warn", result.stderr)
        self.assertEqual(self.budget_text(), before)

    def test_update_on_an_already_pinned_file_changes_no_budget(self) -> None:
        self.write_probe(BASE_VALUES)
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        written = json.loads(self.budget_text())
        for name, value in BASE_VALUES.items():
            self.assertEqual(written["entries"][name]["budget"], value)


class BuildConfigTests(GateTestCase):
    """A Debug build is a different claim, and the gate must say so instead of comparing across."""

    def test_a_mismatched_build_configuration_is_refused(self) -> None:
        self.write_probe(BASE_VALUES)
        document = budget_document(BASE_VALUES)
        document["measurement"]["build_config"] = "Release"
        self.write_budget(document)
        # Rewrite the probe to report Debug while the budget pins Release.
        self.probe.write_text(
            probe_script(self.root / "probe-run-count", BASE_VALUES, config="Debug"),
            encoding="utf-8",
        )
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("recorded from Release", result.stdout)
        self.assertIn("built as Debug", result.stdout)

    def test_a_probe_that_reports_no_configuration_is_refused(self) -> None:
        # A measurement from an unknown build cannot be compared with a pinned one.
        self.write_probe(BASE_VALUES)
        document = budget_document(BASE_VALUES)
        document["measurement"]["build_config"] = "Release"
        self.write_budget(document)
        self.probe.write_text(
            probe_script(self.root / "probe-run-count", BASE_VALUES, config=""),
            encoding="utf-8",
        )
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("did not report its build configuration", result.stdout)

    def test_update_records_the_build_configuration(self) -> None:
        self.write_probe(BASE_VALUES)
        document = budget_document(BASE_VALUES)
        document["measurement"] = {"nodes": 1}  # no configuration yet
        self.write_budget(document)
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        written = json.loads(self.budget_text())
        self.assertEqual(written["measurement"]["build_config"], CONFIG)


class MalformedInputTests(GateTestCase):
    def test_a_missing_probe_is_a_setup_error_not_a_pass(self) -> None:
        # No probe written at all.
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not built", result.stderr)

    def test_a_budget_entry_with_no_measurement_is_an_error(self) -> None:
        # The probe stops reporting `sizeof_node`, so the budget entry has nothing to compare.
        partial = {name: value for name, value in BASE_VALUES.items() if name != "sizeof_node"}
        self.write_probe(partial)
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no measurement", result.stdout)

    def test_an_unknown_status_is_rejected(self) -> None:
        document = budget_document(BASE_VALUES)
        document["entries"]["sizeof_node"]["status"] = "block"
        self.write_probe(BASE_VALUES)
        self.write_budget(document)
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("status", result.stderr)

    def test_a_malformed_budget_file_is_rejected(self) -> None:
        self.write_probe(BASE_VALUES)
        self.budget.write_text("{not json", encoding="utf-8")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not valid JSON", result.stderr)

    def test_a_better_percent_of_100_is_rejected(self) -> None:
        # At 100% the lower bound is 0 and no entry could ever be stale, so half the ratchet dies.
        document = budget_document(BASE_VALUES)
        document["tolerance"]["default"] = {"better": 100.0, "worse": 10.0}
        self.write_probe(BASE_VALUES)
        self.write_budget(document)
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("under 100", result.stderr)

    def test_a_generous_worse_band_is_allowed(self) -> None:
        # The worse side may be a multiple: that is the only band a sampled row can use.
        document = budget_document(
            BASE_VALUES,
            status_by_row={"resident_bytes_per_node": "warn"},
            tolerance={
                "default": {"better": 0.0, "worse": 0.0},
                "resident_bytes_per_node": {"better": 20.0, "worse": 300.0},
            },
        )
        self.write_probe(BASE_VALUES)
        self.write_budget(document)
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class SizesOnlyTests(GateTestCase):
    def test_sizes_only_skips_the_sampled_rows_instead_of_failing_them(self) -> None:
        # The probe still prints the sampled rows; --sizes-only must not compare them, so a moved
        # resident row cannot fail a sizes-only run.
        values = dict(BASE_VALUES)
        values["resident_bytes_per_node"] = 999999
        self.write_probe(values)
        self.write_budget(budget_document(BASE_VALUES))
        result = self.run_gate("--check", "--sizes-only")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("resident_bytes_per_node", result.stdout)


class RatchetUnitTests(unittest.TestCase):
    """The pure band arithmetic, tested without a subprocess."""

    def test_bounds_are_two_sided(self) -> None:
        lower, upper = ratchet.bounds_of(100.0, 20.0, 300.0)
        self.assertAlmostEqual(lower, 80.0)
        self.assertAlmostEqual(upper, 400.0)

    def test_a_zero_tolerance_band_is_exact(self) -> None:
        lower, upper = ratchet.bounds_of(312.0, 0.0, 0.0)
        self.assertEqual((lower, upper), (312.0, 312.0))

    def test_classify_marks_both_sides(self) -> None:
        entry = {"budget": 100, "status": "enforce", "unit": "bytes", "measures": "x"}
        tolerance = {"default": {"better": 20.0, "worse": 300.0}}
        self.assertEqual(ratchet.classify("row", entry, 100, tolerance)[0], "ok")
        self.assertEqual(ratchet.classify("row", entry, 401, tolerance)[0], "regressed")
        self.assertEqual(ratchet.classify("row", entry, 79, tolerance)[0], "stale")

    def test_an_unmeasured_entry_is_out_of_scope_for_compare(self) -> None:
        entries = {
            "measured": {"budget": 10, "status": "enforce", "unit": "bytes", "measures": "x"},
            "missing": {
                "status": "unmeasured",
                "unit": "bytes",
                "measures": "y",
                "reason": "not measurable",
            },
        }
        errors, warnings, notes = ratchet.compare(entries, {"measured": 10}, {"default": 0})
        self.assertEqual(errors, [])
        self.assertEqual(warnings, [])
        self.assertEqual(len(notes), 1)


class TestRealProbe(unittest.TestCase):
    """The committed budget against a real build, when one exists.

    Skipped without a build tree: this is the CMake/ctest lane's job there. Where a build exists it
    proves the fixture above and the real probe agree on the row *names*, which is the failure the
    pure-Python tests cannot see.
    """

    def test_the_committed_budget_lists_what_the_probe_prints(self) -> None:
        build_dir = measure.default_build_dir()
        if build_dir is None:
            self.skipTest("no configured build directory")
        try:
            probe = measure.find_probe(build_dir)
        except measure.SetupError:
            self.skipTest("the memory probe is not built")
        values, config = measure.run_probe_with_config(probe, nodes=64, idle_cycles=1)
        document = json.loads(
            (REPO_ROOT / "tests" / "golden" / "perf" / "memory_budget.json").read_text(
                encoding="utf-8"
            )
        )
        ratchet.validate(document)
        # The probe must report a configuration, and the budget must pin one: the gate refuses to
        # compare a Debug measurement against a Release budget, so the file records which it is.
        self.assertTrue(config, "the probe printed no build configuration")
        self.assertTrue(
            document["measurement"].get("build_config"),
            "the committed budget has no `measurement.build_config`",
        )
        pinned = {
            name for name, entry in document["entries"].items() if entry["status"] != "unmeasured"
        }
        # Every entry the gate checks must have a row, and every row that is a budget quantity must
        # be pinned. The probe also prints some diagnostic rows it does not budget
        # (`plain_node_bytes`, the per-table allocation counts, the resident total); those are
        # deliberately unpinned, so the assertion is one-directional plus a check that the exact rows
        # are all present.
        self.assertEqual(pinned - set(values), set(), "the probe is missing a budgeted row")
        self.assertEqual(set(measure.DETERMINISTIC_ROWS) - set(values), set())


if __name__ == "__main__":
    unittest.main()
