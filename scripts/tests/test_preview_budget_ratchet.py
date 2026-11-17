#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Arithmetic self-tests for scripts/preview_budget_ratchet.py (issue #170, stage P6).

    python -B -m unittest discover -s scripts/tests -p `test_*.py` -v

The band arithmetic and the budget file's contract, tested directly instead of through a subprocess:
exact boundaries (a value exactly on a bound, a value one thousandth outside it) are easier to pin on
the pure functions, and the CLI half -- exit codes, the file write, the refusal to loosen -- is
covered in `test_check_preview_budget.py`.

The cases are the ones that decide whether the ratchet has two live sides:

* the band is computed from two independent percentages, so a generous worse side does not kill the
  staleness side (the single-symmetric-band design did exactly that: at 300% the lower bound is
  negative and no budget could ever be stale),
* `better` is refused at 100% for the same reason,
* an enforced entry may not be raised even inside its tolerance, and the refusal is uniform across
  `warn` and `enforce` so `--update` cannot launder a regression,
* a warned regression is a warning, an unmeasured entry is an error.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent

sys.path.insert(0, str(SCRIPTS_DIR))
import preview_budget_ratchet as ratchet  # noqa: E402  (sibling script, stdlib only)


class RatchetArithmeticTests(unittest.TestCase):
    """The band arithmetic directly, without a subprocess: exact boundaries are easier to pin here."""

    TOLERANCE = {"default": {"better": 10.0, "worse": 10.0}}

    def entry(self, budget: float, status: str = "enforce") -> dict:
        return {"budget": budget, "status": status, "unit": "u", "measures": "x"}

    def test_bounds_are_symmetric_when_both_sides_match(self) -> None:
        lower, upper = ratchet.bounds_of(100.0, 10.0, 10.0)
        self.assertAlmostEqual(lower, 90.0)
        self.assertAlmostEqual(upper, 110.0)

    def test_the_two_sides_are_independent(self) -> None:
        # The real budget's shape: generous on the worse side, meaningful on the better side. This is
        # the case a single symmetric percentage cannot express.
        lower, upper = ratchet.bounds_of(100.0, 20.0, 300.0)
        self.assertAlmostEqual(lower, 80.0)
        self.assertAlmostEqual(upper, 400.0)

    def test_a_value_exactly_on_either_bound_is_ok(self) -> None:
        # Boundary-inclusive on purpose: the band is "within tolerance", and excluding an exact hit
        # would make a pinned round number fail against itself.
        for measured in (90.0, 100.0, 110.0):
            verdict, _ = ratchet.classify("e", self.entry(100.0), measured, self.TOLERANCE)
            self.assertEqual(verdict, "ok", measured)

    def test_just_outside_either_bound_is_caught(self) -> None:
        self.assertEqual(
            ratchet.classify("e", self.entry(100.0), 110.001, self.TOLERANCE)[0], "regressed"
        )
        self.assertEqual(
            ratchet.classify("e", self.entry(100.0), 89.999, self.TOLERANCE)[0], "stale"
        )

    def test_per_entry_tolerance_overrides_the_default(self) -> None:
        tolerance = {"default": {"better": 10.0, "worse": 10.0}, "e": {"better": 50.0, "worse": 50.0}}
        self.assertEqual(ratchet.classify("e", self.entry(100.0), 150.0, tolerance)[0], "ok")
        self.assertEqual(ratchet.classify("e", self.entry(100.0), 135.0, self.TOLERANCE)[0], "regressed")

    def test_a_bare_number_is_a_symmetric_band(self) -> None:
        self.assertEqual(ratchet.tolerance_for("e", {"default": 10.0}), (10.0, 10.0))

    def test_a_zero_tolerance_makes_any_change_fail(self) -> None:
        tolerance = {"default": 0.0}
        self.assertEqual(ratchet.classify("e", self.entry(100.0), 100.0, tolerance)[0], "ok")
        self.assertEqual(ratchet.classify("e", self.entry(100.0), 101.0, tolerance)[0], "regressed")

    def test_an_enforced_entry_is_refused_a_raise_even_inside_the_tolerance(self) -> None:
        # 101 is inside the 10% band, but a budget still may not grow: the tolerance covers
        # measurement variation, and "inside the band" is not permission to make the module bigger.
        self.assertEqual(
            ratchet.refused_by_update({"e": self.entry(100.0)}, {"e": 101.0}), [
                "e: would raise the enforce budget from 100 to 101 u"
            ]
        )

    def test_a_warned_entry_is_also_refused_a_raise(self) -> None:
        # A warned latency row re-pinned on a loaded box records contention as the budget. Refusing
        # the raise for both statuses is what keeps --update from being a way to launder a regression.
        self.assertEqual(
            ratchet.refused_by_update({"e": self.entry(100.0, "warn")}, {"e": 101.0}), [
                "e: would raise the warn budget from 100 to 101 u"
            ]
        )

    def test_a_tightening_is_never_refused(self) -> None:
        for status in ("warn", "enforce"):
            self.assertEqual(
                ratchet.refused_by_update({"e": self.entry(100.0, status)}, {"e": 99.0}), []
            )

    def test_compare_puts_a_warned_regression_in_warnings_not_errors(self) -> None:
        errors, warnings, notes = ratchet.compare(
            {"e": self.entry(100.0, "warn")}, {"e": 200.0}, self.TOLERANCE
        )
        self.assertEqual(errors, [])
        self.assertEqual(len(warnings), 1)
        self.assertEqual(notes, [])

    def test_compare_an_unmeasured_entry_is_an_error(self) -> None:
        errors, _warnings, _notes = ratchet.compare({"e": self.entry(100.0)}, {}, self.TOLERANCE)
        self.assertEqual(len(errors), 1)
        self.assertIn("no measurement", errors[0])

    def test_validate_rejects_a_wrong_schema_and_a_missing_tolerance(self) -> None:
        good = {
            "schema": 1,
            "tolerance": {"default": {"better": 10.0, "worse": 10.0}},
            "entries": {"e": self.entry(100.0)},
        }
        self.assertIs(ratchet.validate(good), good)
        for broken in (
            {},
            {"schema": 2, "tolerance": {"default": 1}, "entries": {"e": self.entry(1)}},
            {"schema": 1, "entries": {"e": self.entry(1)}},
            {"schema": 1, "tolerance": {"x": 1}, "entries": {"e": self.entry(1)}},
            {"schema": 1, "tolerance": {"default": 1}, "entries": {}},
            {"schema": 1, "tolerance": {"default": 1}, "entries": {"e": {"budget": 1}}},
            {"schema": 1, "tolerance": {"default": 1}, "entries": {"e": self.entry(0)}},
            # A better-side allowance of 100 makes the lower bound zero, which would disable the
            # staleness half of the ratchet; the validator must refuse it rather than accept it.
            {
                "schema": 1,
                "tolerance": {"default": {"better": 100.0, "worse": 10.0}},
                "entries": {"e": self.entry(1)},
            },
            {
                "schema": 1,
                "tolerance": {"default": {"worse": 10.0}},
                "entries": {"e": self.entry(1)},
            },
        ):
            with self.assertRaises(ratchet.UsageError):
                ratchet.validate(broken)

    def test_repinned_rounds_to_three_decimals_and_touches_only_measured_entries(self) -> None:
        document = {
            "schema": 1,
            "tolerance": {"default": 1.0},
            "entries": {"a": self.entry(1.0), "b": self.entry(2.0)},
        }
        updated = ratchet.repinned(document, {"a": 0.0366666}, "enc", {"statistic": "min"})
        self.assertEqual(updated["entries"]["a"]["budget"], 0.037)
        self.assertEqual(updated["entries"]["b"]["budget"], 2.0)
        self.assertEqual(updated["encoder"], "enc")
        # The input document must not be mutated: a refused update may still have rebuilt it.
        self.assertEqual(document["entries"]["a"]["budget"], 1.0)

    def test_update_notes_report_only_tightenings(self) -> None:
        notes = ratchet.update_notes(
            {"a": self.entry(100.0), "b": self.entry(100.0)}, {"a": 90.0, "b": 120.0}
        )
        self.assertEqual(notes, ["a: tightened from 100 to 90 u"])

    # --- the recorded breakdown (issue #395) ---------------------------------------------------

    def recorded_entry(self, recorded: dict) -> dict:
        entry = self.entry(100.0)
        entry["unit"] = "bytes"
        entry["recorded"] = recorded
        return entry

    def test_recorded_breakdown_names_every_gzip_row_but_the_entry_itself(self) -> None:
        # The total is not a component of itself, and a duration is not a file: the breakdown is the
        # per-file gzip rows and nothing else.
        measurements = {
            "a.js.gzip": 100.0,
            "a.wasm.gzip": 200.0,
            "download_gzip_bytes": 300.0,
            "cold_engine_ready_ms": 15.0,
        }
        self.assertEqual(
            ratchet.recorded_breakdown({"download_gzip_bytes": {}}, measurements),
            {"a.js.gzip": 100.0, "a.wasm.gzip": 200.0},
        )

    def test_a_breakdown_that_disagrees_with_the_measurement_is_an_error(self) -> None:
        # The attribution is what makes a growth reviewable: a block that names the wrong size tells
        # the reviewer the wrong file grew, even when the total is inside its band.
        errors = ratchet.breakdown_drift(
            {"e": self.recorded_entry({"a.js.gzip": 1.0})}, {"a.js.gzip": 100.0, "e": 100.0}
        )
        self.assertEqual(len(errors), 1)
        self.assertIn("a.js.gzip is 1", errors[0])
        self.assertIn("re-record it with --update", errors[0])

    def test_a_file_the_breakdown_does_not_name_is_an_error(self) -> None:
        errors = ratchet.breakdown_drift(
            {"e": self.recorded_entry({"a.js.gzip": 100.0})},
            {"a.js.gzip": 100.0, "b.wasm.gzip": 7.0, "e": 107.0},
        )
        self.assertEqual(len(errors), 1)
        self.assertIn("does not name b.wasm.gzip", errors[0])
        self.assertIn("unattributed", errors[0])

    def test_a_file_the_measurement_dropped_is_an_error(self) -> None:
        # The other direction: a breakdown naming a file that no longer ships is attribution that has
        # outlived its asset.
        errors = ratchet.breakdown_drift(
            {"e": self.recorded_entry({"a.js.gzip": 100.0, "gone.wasm.gzip": 5.0})},
            {"a.js.gzip": 100.0, "e": 100.0},
        )
        self.assertEqual(len(errors), 1)
        self.assertIn("no longer weighs", errors[0])

    def test_a_matching_breakdown_has_no_errors(self) -> None:
        measurements = {"a.js.gzip": 100.0, "e": 100.0}
        self.assertEqual(
            ratchet.breakdown_drift({"e": self.recorded_entry({"a.js.gzip": 100.0})}, measurements),
            [],
        )

    def test_update_refreshes_the_breakdown_from_the_same_measurement(self) -> None:
        # The pin and the attribution must come from one run: a hand-copied breakdown drifts from the
        # total the first time someone re-pins and forgets.
        document = {
            "schema": 1,
            "tolerance": {"default": 1.0},
            "entries": {"e": self.recorded_entry({"a.js.gzip": 1.0})},
        }
        updated = ratchet.repinned(
            document, {"a.js.gzip": 100.0, "b.wasm.gzip": 5.0, "e": 105.0}, "enc", {}
        )
        self.assertEqual(
            updated["entries"]["e"]["recorded"], {"a.js.gzip": 100.0, "b.wasm.gzip": 5.0}
        )
        self.assertEqual(updated["entries"]["e"]["budget"], 105.0)
        # The input is untouched, like the pin computation itself.
        self.assertEqual(document["entries"]["e"]["recorded"], {"a.js.gzip": 1.0})

    def test_an_entry_without_a_breakdown_is_left_alone(self) -> None:
        # Latency entries have no files; `--update` must not invent a breakdown for them.
        document = {"schema": 1, "tolerance": {"default": 1.0}, "entries": {"e": self.entry(1.0)}}
        updated = ratchet.repinned(document, {"kind": 2.0, "e": 1.0}, "enc", {})
        self.assertNotIn("recorded", updated["entries"]["e"])


if __name__ == "__main__":
    unittest.main()
