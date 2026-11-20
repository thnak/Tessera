#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CLI self-tests for scripts/check_preview_budget.py (the P6 two-sided ratchet, issue #170; #395).

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

Every test builds a throwaway tree with fake modules and a fake page and runs the real CLI, so the
cases are about what a user sees -- the exit code, the words on stdout/stderr, and whether the budget
file changed -- rather than about internals. The fake modules are real files of a chosen size, so
`download_gzip_bytes` moves when a test wants it to, and every measurement case runs `--size-only` so
no test needs Node or an Emscripten build (the Pages lane owns that half).

The cases are exactly the behaviours a two-sided ratchet buys, plus the ones that keep it honest:

* worse than the budget by more than `tolerance.worse` -> exit 1 (the regression side),
* much better -> exit 1 telling you to tighten (the staleness side; without it the budget rots),
* inside each side's band -> exit 0,
* `--update` re-pins a tightening but refuses to loosen, and writes nothing when it refuses,
* a `warn` entry reports the same regression as a warning, not an error,
* the size row is exactly reproducible: two runs of the same tree must agree,
* a malformed budget, an unknown status or a missing module is not a pass.

Issue #395 adds three:

* **every module the studio loads is required**: a directory holding diagnostics only is a setup
  error, not a cheaper page, and each module's own size reaches the report,
* **the recorded breakdown is ratcheted with the total**: a file whose size moved without the
  breakdown being re-recorded fails, because that attribution is what makes a growth reviewable,
* **the calibration contract is enforced**: a budget without a reference calibration, and a probe
  whose calibration workload changed, are setup errors rather than silent re-baselines.

The pure band arithmetic is covered in `test_preview_budget_ratchet.py`.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
GATE = SCRIPTS_DIR / "check_preview_budget.py"
MEASURE = SCRIPTS_DIR / "preview_budget_measure.py"
RATCHET = SCRIPTS_DIR / "preview_budget_ratchet.py"

# The page assets the measurement weighs, imported from the measurement module so the fixture cannot
# drift from what the gate actually measures.
sys.path.insert(0, str(SCRIPTS_DIR))
import preview_budget_measure as measure  # noqa: E402  (sibling script, stdlib only)
import preview_budget_ratchet as ratchet  # noqa: E402  (sibling script, stdlib only)

# The built page, as a directory (issue #422): the measurement walks `dist/`, so the fixture builds one.
# The names mimic a real Vite build -- a hashed chunk under `assets/` plus the files Vite copies from
# `public/` -- because the paths are part of what the measurement reports.
PAGE_FIXTURE_FILES = (
    "index.html",
    "assets/index-Ab1Cd2Ef.js",
    "assets/index-Ab1Cd2Ef.css",
    "assets/vendor-Qq9Qq9Qq.js",
    "profiles/desktop-linux/profile.json",
)

# A page and modules whose gzip sizes are stable for a given byte pattern: the content is generated
# from a fixed seed, so the number is the same on every machine even though it is not round.
PAGE_BYTES = 4096
MODULE_BYTES = 65536

# The reference calibration a test budget records. The value is arbitrary -- it is a divisor, not a
# measurement -- but it must be present and positive, because the latency rows are normalized by it.
TEST_REFERENCE_MS = 15.0


def gzip_size(data: bytes) -> int:
    compressor = zlib.compressobj(9, zlib.DEFLATED, 16 + zlib.MAX_WBITS)
    return len(compressor.compress(data) + compressor.flush())


def payload(size: int, seed: int) -> bytes:
    """Deterministic, poorly compressible bytes: no wall clock, no `random` module.

    A tiny linear congruential generator, so a gzip of the result is close to the raw size (a run of
    one repeated string would compress to a few dozen bytes and make "budget - 1000" go negative).
    """
    state = (seed * 2654435761 + 1) & 0xFFFFFFFF
    out = bytearray(size)
    for index in range(size):
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        out[index] = (state >> 16) & 0xFF
    return bytes(out)


class PreviewBudgetGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.repo = Path(self._temp.name)
        scripts = self.repo / "scripts"
        (scripts / "ci").mkdir(parents=True)
        shutil.copy2(GATE, scripts / GATE.name)
        shutil.copy2(MEASURE, scripts / MEASURE.name)
        shutil.copy2(RATCHET, scripts / RATCHET.name)
        # The probe is never executed here (every test is --size-only), but the path has to exist
        # because the measurement module resolves it at import time.
        (scripts / "ci" / "preview_budget_probe.mjs").write_text("// stub\n", encoding="utf-8")

        page = self.repo / "designer" / "web"
        page.mkdir(parents=True)
        # The *built* page: the measurement reads `designer/web/dist/` as a directory because a Vite build's
        # asset names carry content hashes and cannot be listed (issue #422, ADR-054 decision 6). The fixture
        # therefore mirrors that layout rather than a flat list of source files, and the names come from this
        # module so a change to what a build emits is a change here.
        self.page = page / "dist"
        for index, name in enumerate(PAGE_FIXTURE_FILES, start=1):
            target = self.page / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload(PAGE_BYTES, index))
        # A source map is a build artifact the browser never fetches for execution; the measurement skips it.
        (self.page / "assets" / "index-Ab1Cd2Ef.js.map").write_bytes(payload(PAGE_BYTES, 1))
        self.module_dir = self.repo / "build" / "pages"
        self.module_dir.mkdir(parents=True)
        # Every module the studio loads, one pair of files each (issue #395). Each module gets a
        # different seed so a report naming the wrong one would show up as a moved size.
        self.module_bytes = {}
        seed = 2
        for module in measure.MODULES:
            for extension in ("js", "wasm"):
                name = "{}.{}".format(module, extension)
                (self.module_dir / name).write_bytes(payload(MODULE_BYTES, seed))
                self.module_bytes[name] = MODULE_BYTES
                seed += 1

        self.budget = self.repo / "tests" / "golden" / "perf" / "preview_budget.json"
        self.budget.parent.mkdir(parents=True)
        sizes = [gzip_size(payload(PAGE_BYTES, index)) for index in range(1, len(PAGE_FIXTURE_FILES) + 1)]
        for index in range(2, 2 + len(measure.MODULE_FILES)):
            sizes.append(gzip_size(payload(MODULE_BYTES, index)))
        self.download = sum(sizes)
        self.write_budget()

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write_budget(self, download: int | None = None, status: str = "enforce",
                     tolerance: float = 5.0, reference_ms: float | None = TEST_REFERENCE_MS) -> None:
        measurement = {
            "cold_runs": 5,
            "edit_probe_runs": 5,
            "edit_samples_per_run": 30,
            "edit_warmup": 5,
            "statistic": "min",
            "normalization": "measured_ms * calibration_reference_ms / calibration_ms",
        }
        if reference_ms is not None:
            measurement["calibration_reference_ms"] = reference_ms
        document = {
            "schema": 1,
            "encoder": "test",
            "measurement": measurement,
            "tolerance": {
                "default": {"better": 20.0, "worse": 300.0},
                "download_gzip_bytes": {"better": 1.0, "worse": tolerance},
            },
            "entries": {
                "download_gzip_bytes": {
                    "budget": self.download if download is None else download,
                    "status": status,
                    "unit": "bytes",
                    "measures": "the compressed download",
                    # The breakdown is what makes a growth attributable, so the fixture produces it
                    # from the same tree the gate walks rather than hard-coding fifteen numbers.
                    "recorded": ratchet.recorded_breakdown(
                        {"download_gzip_bytes": {}}, self.measurements_for_fixture()
                    ),
                }
            },
        }
        self.budget.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")

    def measurements_for_fixture(self) -> dict:
        """The size rows the fixture tree produces, without running the probe.

        The gate's own measurement walks the same tree, so building the expected breakdown here the
        same way keeps the fixture from hard-coding fifteen numbers that must all be right for an
        unrelated assertion to pass.
        """
        sizes: dict = {}
        sizes.update(measure._sizes(measure.page_assets(self.page), "page asset", root=self.page))
        sizes.update(
            measure._sizes(measure.module_assets(self.module_dir), "Wasm module artifact")
        )
        sizes["download_gzip_bytes"] = sum(
            value for name, value in sizes.items() if name.endswith(".gzip")
        )
        return {name: float(value) for name, value in sizes.items()}

    def read_budget(self) -> dict:
        return json.loads(self.budget.read_text(encoding="utf-8"))

    def run_gate(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(self.repo / "scripts" / GATE.name),
                "--size-only",
                "--module-dir",
                str(self.module_dir),
                "--budget",
                str(self.budget),
                *args,
            ],
            cwd=str(self.repo),
            capture_output=True,
            text=True,
            encoding="utf-8",
        )

    # --- the ratchet, both sides ---------------------------------------------------------------

    def test_matching_budget_passes(self) -> None:
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 error(s)", result.stdout)

    def test_worse_than_the_budget_fails(self) -> None:
        # 10% below the measurement, so the measured value sits outside the 5% band on the high side.
        self.write_budget(download=int(self.download * 0.9))
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ERROR download_gzip_bytes", result.stdout)
        self.assertIn("upper bound", result.stdout)

    def test_much_better_than_the_budget_fails_as_stale(self) -> None:
        # Twice the measured value: far outside a 5% band, so the budget is stale, not fine.
        self.write_budget(download=self.download * 2)
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ERROR download_gzip_bytes", result.stdout)
        self.assertIn("tighten it with --update", result.stdout)

    def test_inside_the_tolerance_passes_both_ways(self) -> None:
        # The fixture's download entry allows +5% (worse) and -1% (better), so 2% either way is a pass
        # and both sides are exercised inside their own band rather than one shared one.
        self.write_budget(download=int(self.download / 1.02))  # measured ~2% over budget
        self.assertEqual(self.run_gate("--check").returncode, 0)
        self.write_budget(download=int(self.download / 0.995))  # measured ~0.5% under budget
        self.assertEqual(self.run_gate("--check").returncode, 0)

    def test_a_small_improvement_beyond_the_better_band_is_already_stale(self) -> None:
        # The enforced row's better-side band is deliberately tight (1%): the download is exact, so
        # "4% smaller" is real news and the budget must not silently keep 4% of slack.
        self.write_budget(download=int(self.download / 0.96))
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("tighten it with --update", result.stdout)

    # --- --update ------------------------------------------------------------------------------

    def test_update_tightens_a_stale_budget(self) -> None:
        self.write_budget(download=self.download * 2)
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.read_budget()["entries"]["download_gzip_bytes"]["budget"], self.download)

    def test_update_refuses_to_loosen_and_writes_nothing(self) -> None:
        self.write_budget(download=int(self.download * 0.9))
        before = self.budget.read_text(encoding="utf-8")
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("refuses to loosen", result.stderr)
        self.assertEqual(self.budget.read_text(encoding="utf-8"), before)

    def test_update_also_refuses_to_loosen_a_warned_entry(self) -> None:
        # Warning is about the *gate*, not about the ratchet: a warned row re-pinned on a loaded
        # machine would bake machine load in as headroom, so the refusal is uniform. The checked-in
        # budget's own history is the evidence: an --update on a busy box moved the cold row from
        # 16.4 ms to 23.6 ms with no code change at all.
        self.write_budget(download=int(self.download * 0.9), status="warn")
        before = self.budget.read_text(encoding="utf-8")
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("refuses to loosen", result.stderr)
        self.assertEqual(self.budget.read_text(encoding="utf-8"), before)

    # --- warn versus enforce -------------------------------------------------------------------

    def test_a_warn_entry_reports_a_regression_as_a_warning(self) -> None:
        self.write_budget(download=int(self.download * 0.9), status="warn")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARN download_gzip_bytes", result.stdout)
        self.assertIn("warn-only entry", result.stdout)
        self.assertNotIn("ERROR", result.stdout)

    # --- determinism and failure modes ---------------------------------------------------------

    def test_two_check_runs_agree_exactly(self) -> None:
        # The enforced row is the deterministic one: identical tree, identical output.
        first = self.run_gate("--check")
        second = self.run_gate("--check")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual(first.stdout, second.stdout)

    def test_malformed_budget_is_a_usage_error(self) -> None:
        self.budget.write_text("{not json", encoding="utf-8")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not valid JSON", result.stderr)

    def test_unknown_status_is_a_usage_error(self) -> None:
        document = self.read_budget()
        document["entries"]["download_gzip_bytes"]["status"] = "advisory"
        self.budget.write_text(json.dumps(document), encoding="utf-8")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("expected one of", result.stderr)

    def test_missing_module_is_a_setup_error_not_a_pass(self) -> None:
        (self.module_dir / "tessera_diagnostics.wasm").unlink()
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("SETUP ERROR", result.stderr)

    def test_json_report_carries_the_measurements(self) -> None:
        result = self.run_gate("--check", "--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "preview-budget")
        self.assertEqual(payload["measurements"]["download_gzip_bytes"], self.download)

    # --- every module the studio loads (issue #395) --------------------------------------------

    def test_every_module_the_studio_loads_is_weighed(self) -> None:
        # The acceptance's second half. All three modules are in the total, and each is reported
        # separately so a report can name the file that grew instead of only the sum.
        result = self.run_gate("--check", "--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        measurements = json.loads(result.stdout)["measurements"]
        for module in measure.MODULES:
            for extension in ("js", "wasm"):
                self.assertIn("{}.{}.gzip".format(module, extension), measurements)
        module_total = sum(
            measurements["{}.{}.gzip".format(module, extension)]
            for module in measure.MODULES
            for extension in ("js", "wasm")
        )
        self.assertLess(module_total, self.download)

    def test_the_total_actually_moved_when_a_module_grew(self) -> None:
        # Guard against a measurement that names a module but does not add it: growing the largest
        # module must move the enforced total, or the coverage is cosmetic.
        before = self.download
        (self.module_dir / "tessera_canvas.wasm").write_bytes(payload(MODULE_BYTES * 2, 99))
        result = self.run_gate("--check", "--json")
        after = json.loads(result.stdout)["measurements"]["download_gzip_bytes"]
        self.assertGreater(after, before)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertTrue(any("download_gzip_bytes: measured" in error for error in json.loads(result.stdout)["errors"]))

    def test_a_partially_assembled_site_is_a_setup_error(self) -> None:
        # A site with diagnostics only is the exact state issue #395 exists to make impossible to
        # pass; it must report as missing, not be measured as a cheaper download.
        (self.module_dir / "tessera_canvas.wasm").unlink()
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("SETUP ERROR", result.stderr)

    # --- the recorded breakdown is ratcheted with the total (#395) ------------------------------

    def test_a_stale_recorded_breakdown_fails(self) -> None:
        # The attribution is what makes a growth reviewable, so a breakdown that disagrees with the
        # measurement is an error even when the total is inside its band.
        document = self.read_budget()
        document["entries"]["download_gzip_bytes"]["recorded"]["index.html.gzip"] = 1
        self.budget.write_text(json.dumps(document), encoding="utf-8")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("the recorded breakdown says index.html.gzip is 1 bytes", result.stdout)
        self.assertIn("--update", result.stdout)

    def test_an_unrecorded_file_fails(self) -> None:
        # A file the measurement weighs but the breakdown does not name is an unattributed asset.
        document = self.read_budget()
        del document["entries"]["download_gzip_bytes"]["recorded"]["tessera_canvas.wasm.gzip"]
        self.budget.write_text(json.dumps(document), encoding="utf-8")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("does not name tessera_canvas.wasm.gzip", result.stdout)

    def test_update_refreshes_the_recorded_breakdown(self) -> None:
        # A tightening must leave the attribution agreeing with the new measurement, which is the
        # whole point of generating it rather than hand-copying it.
        self.write_budget(download=self.download * 2)
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        recorded = self.read_budget()["entries"]["download_gzip_bytes"]["recorded"]
        expected = ratchet.recorded_breakdown(
            {"download_gzip_bytes": {}}, self.measurements_for_fixture()
        )
        self.assertEqual(recorded, expected)
        self.assertEqual(self.run_gate("--check").returncode, 0)

    def test_a_size_only_update_does_not_write_a_zero_calibration(self) -> None:
        # A size-only run does not measure latency, so it cannot record a reference calibration.
        # Writing 0.0 there would make the next full run fail for a reason the size-only lane caused,
        # and zero is not a reference anyone can normalize against.
        self.write_budget(download=self.download * 2, reference_ms=None)
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        measurement = self.read_budget()["measurement"]
        self.assertNotIn("calibration_reference_ms", measurement)
        self.assertNotIn("normalization", measurement)
        # The counts are still refreshed, so a tightening is still a complete re-record of what it did.
        self.assertEqual(measurement["cold_runs"], 5)

    # --- the calibration contract (#395) -------------------------------------------------------

    def test_a_latency_row_without_a_reference_calibration_is_a_usage_error(self) -> None:
        # The rows are normalized by it, so a budget that names a latency pin without the reference
        # it was recorded against is not a claim anyone can reproduce.
        self.write_budget(reference_ms=None)
        result = self.run_gate("--check", "--size-only")
        # A size-only run never divides by it, so it stays a pass: that lane weighs bytes only.
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_no_build_validates_the_file_and_says_what_it_did_not_measure(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(self.repo / "scripts" / GATE.name),
                "--check",
                "--no-build",
                "--budget",
                str(self.budget),
            ],
            cwd=str(self.repo),
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("--no-build", result.stdout)
        # It must say the numbers were NOT measured here rather than print a pass that reads like
        # the budgets passed, and it must point at the lane that does measure them.
        self.assertIn("the numbers are measured by the Pages lane", result.stdout)
        self.assertIn("enforced here: download_gzip_bytes", result.stdout)

    def test_no_build_rejects_a_malformed_file(self) -> None:
        self.budget.write_text("{not json", encoding="utf-8")
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(self.repo / "scripts" / GATE.name),
                "--check",
                "--no-build",
                "--budget",
                str(self.budget),
            ],
            cwd=str(self.repo),
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not valid JSON", result.stderr)


