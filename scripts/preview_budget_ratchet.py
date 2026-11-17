#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ratchet policy half of the preview budget gate (issue #170, stage P6).

Pure functions over numbers: no filesystem, no subprocess, no clock. `check_preview_budget.py` is the
CLI that reads the budget file, calls `preview_budget_measure.py` for the numbers, and applies the
policy here. Splitting it this way is what lets the ratchet's arithmetic be unit-tested directly
instead of only through a subprocess.

## The two-sided ratchet

Each entry is pinned to a number and a pair of tolerances, because the two sides are not the same
kind of claim and a single symmetric band cannot express both. Let
`lower = budget * (1 - better_percent/100)` and `upper = budget * (1 + worse_percent/100)`:

* `measured > upper` -> **regressed**: the budget was exceeded by more than the allowed slack,
* `measured < lower` -> **stale**: the code got much better and nobody recorded it. The budget is now
  slack, and slack is how a budget rots into a no-op one "small win" at a time,
* otherwise -> **ok**.

A single symmetric percentage would quietly kill one side of the ratchet: to be generous enough for a
noisy wall-clock row the allowance has to be a multiple (300% here), and at that point `budget * (1 -
3)` is negative, so *nothing* could ever be stale. Splitting the two sides keeps the regression
allowance generous and the staleness side meaningful at the same time, and both stay expressible per
entry. `better_percent` must be under 100, which is what keeps the lower bound positive.

`--update` re-pins from the current measurements and **refuses to loosen any entry**: one that the
update would have to raise fails and nothing is written, exactly like
`scripts/check_source_size.py --update`. Both sides therefore only ever push a budget down.

The refusal is uniform, applied to `warn` and `enforce` alike, and that is deliberate: a warned latency
entry re-pinned on a loaded box would record contention as the budget. Running `--update` here on a
busy machine moved cold-engine-ready from 16.4 ms to 23.6 ms with no code change at all, which is
budget rot arriving through the tool rather than through a regression. Raising any number -- warned or
enforced -- is an explicit edit of the budget file in a reviewed change, with the why in the diff.
A warned entry that has genuinely regressed therefore keeps warning until a human decides, which is
exactly the attention warn-only is meant to buy.

## Warn versus enforce

`status` is per entry because "warnings first, then ratchets" is the documented fix path for this
slice. An enforced entry turns a stale or exceeded budget into a failed build; a warned entry prints
the same message and passes, so the trend is visible on every run before anyone is blocked by it.
The tolerance is the allowance for *measurement* variation, not for a slower module.
"""

from __future__ import annotations

import copy
from typing import Dict, List, Sequence, Tuple

STATUSES = ("warn", "enforce")


class UsageError(Exception):
    """The budget document does not satisfy the gate's contract."""


def tolerance_for(name: str, tolerance: Dict) -> Tuple[float, float]:
    """`(better_percent, worse_percent)` for one entry.

    A pair, not one number, because the two sides answer different questions: how much improvement
    counts as "much better, record it" and how much slack counts as "worse, fail". A single symmetric
    band wide enough for a noisy timing row would push the better side below zero and disable
    staleness detection entirely.
    """
    value = tolerance.get(name, tolerance["default"])
    if isinstance(value, dict):
        return float(value["better"]), float(value["worse"])
    # A bare number is a symmetric band; accepted so a zero-tolerance row or a test stays readable.
    return float(value), float(value)


def bounds_of(budget: float, better_percent: float, worse_percent: float) -> Tuple[float, float]:
    """The accepted band `(lower, upper)`. Lower is the stale side, upper the regression side."""
    return budget * (1.0 - better_percent / 100.0), budget * (1.0 + worse_percent / 100.0)


def validate_tolerance(tolerance: object) -> Dict:
    """Checks the `tolerance` block: a default, and every value a non-negative number or a pair."""
    if not isinstance(tolerance, dict) or "default" not in tolerance:
        raise UsageError("the budget file has no `tolerance.default`")

    def check(label: str, value: object) -> None:
        if isinstance(value, dict):
            for side in ("better", "worse"):
                if side not in value:
                    raise UsageError("{}.{} has no {!r}".format(label, side, side))
                check_value("{}.{}".format(label, side), value[side])
            if float(value["better"]) >= 100.0:
                # At 100% the lower bound is zero, so nothing could ever be stale and the ratchet
                # would only have one side -- the exact failure this pair exists to prevent.
                raise UsageError(
                    "{}.better must be under 100 (at 100 the lower bound is 0 and no budget could "
                    "ever be stale)".format(label)
                )
            return
        check_value(label, value)

    def check_value(label: str, value: object) -> None:
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
            raise UsageError("{} must be a non-negative number".format(label))

    check("tolerance.default", tolerance["default"])
    for key, value in sorted(tolerance.items()):
        if key != "default":
            check("tolerance.{}".format(key), value)
    return tolerance


def validate(document: object) -> Dict:
    """Checks the budget document's shape and returns it, or raises `UsageError`.

    Validated strictly and up front: a typo in a key would otherwise silently drop an entry from the
    comparison, which is the same failure mode as a gate that does not run at all.
    """
    if not isinstance(document, dict):
        raise UsageError("the budget file is not a JSON object")
    if document.get("schema") != 1:
        raise UsageError("the budget file has schema {!r}, expected 1".format(document.get("schema")))
    entries = document.get("entries")
    if not isinstance(entries, dict) or not entries:
        raise UsageError("the budget file has no non-empty `entries` object")
    validate_tolerance(document.get("tolerance"))
    for name, entry in sorted(entries.items()):
        if not isinstance(entry, dict):
            raise UsageError("entry {} is not an object".format(name))
        for field in ("budget", "status", "unit", "measures"):
            if field not in entry:
                raise UsageError("entry {} has no {!r}".format(name, field))
        if entry["status"] not in STATUSES:
            raise UsageError(
                "entry {} has status {!r}; expected one of {}".format(name, entry["status"], STATUSES)
            )
        budget = entry["budget"]
        if not isinstance(budget, (int, float)) or isinstance(budget, bool) or budget <= 0:
            raise UsageError("entry {} budget must be a positive number".format(name))
    return document


def amount(value: float, unit: str) -> str:
    """A reading with its unit. Byte counts are exact integers; a millisecond median is not."""
    if float(value).is_integer():
        return "{:.0f} {}".format(value, unit)
    return "{:.3f} {}".format(value, unit)


def classify(name: str, entry: Dict, measured: float, tolerance: Dict) -> Tuple[str, str]:
    """(verdict, message) for one entry; verdict is 'ok', 'regressed' or 'stale'."""
    pinned = float(entry["budget"])
    better, worse = tolerance_for(name, tolerance)
    lower, upper = bounds_of(pinned, better, worse)
    unit = entry.get("unit", "")
    reading = "measured {}, budget {}, tolerance -{:.0f}%/+{:.0f}%".format(
        amount(measured, unit), amount(pinned, unit), better, worse
    )
    if measured > upper:
        return "regressed", "{}: {} (upper bound {})".format(name, reading, amount(upper, unit))
    if measured < lower:
        return "stale", (
            "{}: {} (lower bound {}); the entry is much better than its budget, so tighten it with "
            "--update".format(name, reading, amount(lower, unit))
        )
    return "ok", "{}: {}".format(name, reading)


def compare(
    entries: Dict[str, Dict], measurements: Dict[str, float], tolerance: Dict
) -> Tuple[List[str], List[str], List[str]]:
    """(errors, warnings, notes), each sorted by entry name.

    An entry without a measurement is an error: a budget nobody measured is not a budget that passed.
    """
    errors: List[str] = []
    warnings: List[str] = []
    notes: List[str] = []
    for name in sorted(entries):
        entry = entries[name]
        if name not in measurements:
            errors.append("{}: no measurement for this budget entry".format(name))
            continue
        verdict, message = classify(name, entry, float(measurements[name]), tolerance)
        if verdict == "ok":
            notes.append(message)
        elif entry["status"] == "enforce":
            errors.append(message)
        else:
            warnings.append("{} (warn-only entry)".format(message))
    return errors, warnings, notes


def refused_by_update(
    entries: Dict[str, Dict], measurements: Dict[str, float]
) -> List[str]:
    """The entries an `--update` would have to loosen, so it must write nothing.

    Uniform across `warn` and `enforce`, for the reason in the module docstring: a warned latency
    entry re-pinned on a loaded box records contention as the budget, which is rot arriving through
    the tool. Raising any number is a reviewed edit of the budget file instead.
    """
    refused: List[str] = []
    for name in sorted(entries):
        entry = entries[name]
        if name not in measurements:
            continue
        measured = float(measurements[name])
        pinned = float(entry["budget"])
        if measured > pinned:
            refused.append(
                "{}: would raise the {} budget from {} to {} {}".format(
                    name,
                    entry["status"],
                    "{:g}".format(pinned),
                    "{:g}".format(round(measured, 3)),
                    entry.get("unit", ""),
                )
            )
    return refused


def update_notes(
    entries: Dict[str, Dict], measurements: Dict[str, float]
) -> List[str]:
    """What an `--update` reports: the entries it tightened, and by how much.

    Only tightenings can appear here, because `refused_by_update` has already vetoed the rest. They
    are printed rather than silent so a reviewer sees a budget move in the CI log, not only in the
    diff.
    """
    notes: List[str] = []
    for name in sorted(entries):
        entry = entries[name]
        if name not in measurements:
            continue
        pinned = float(entry["budget"])
        measured = round(float(measurements[name]), 3)
        if measured < pinned:
            notes.append(
                "{}: tightened from {} to {} {}".format(
                    name,
                    "{:g}".format(pinned),
                    "{:g}".format(measured),
                    entry.get("unit", ""),
                )
            )
    return notes


def recorded_breakdown(entries: Dict[str, Dict], measurements: Dict[str, float]) -> Dict[str, float]:
    """The size rows that make up an entry's total, as the measurement reports them.

    A row belongs in the breakdown when it is a gzip size and is not itself a budget entry: the total
    is not a component of itself, and a latency row is a duration rather than a file. Which rows those
    are is therefore decided by the measurement, not by a list kept here that could fall out of date.
    """
    recorded: Dict[str, float] = {}
    for name, value in measurements.items():
        if not name.endswith(".gzip") or name in entries:
            continue
        recorded[name] = round(float(value), 3)
    return dict(sorted(recorded.items()))


def repinned(document: Dict, measurements: Dict[str, float], encoder: str, counts: Dict) -> Dict:
    """The budget document after `--update`: pinned numbers, encoder and sample counts refreshed.

    An entry with a `recorded` block also has it refreshed from the same measurement, so the
    per-file attribution a reviewer reads is the measurement that produced the pin rather than a
    number someone copied out of a terminal once (issue #395: a growth of the download budget has to
    be attributable to a file).
    """
    updated = copy.deepcopy(document)
    updated["encoder"] = encoder
    updated["measurement"] = counts
    breakdown = recorded_breakdown(updated["entries"], measurements)
    for name, entry in updated["entries"].items():
        if name in measurements:
            entry["budget"] = round(float(measurements[name]), 3)
        if isinstance(entry.get("recorded"), dict):
            entry["recorded"] = dict(breakdown)
    return updated


# How far a per-file gzip size may move before the breakdown counts as stale.
#
# The pin is recorded on one host and checked on another (Windows locally, Linux in the Pages lane), and
# zlib's output for the same input legitimately differs between them -- observed 0.03% to 0.52% across
# nine files, in both directions, with no source change at all. Comparing exactly made the drift check
# fail on every row of a correct record, which is worse than not having it: a gate that always fails is
# a gate people switch off.
#
# Sized to swallow encoder variance and nothing else. A genuine growth of one file is far larger, and
# the row it must trip is the *total* ratchet, which is percentage-based and much wider (5%/1% for the
# size row). This one only has to answer "is this still the same file?".
BREAKDOWN_ENCODER_SLACK = 0.015


def same_measurement(recorded: float, measured: float) -> bool:
    """Whether two readings of one file's size are the same measurement.

    Relative, not absolute, because the files span 2 KB to 1.3 MB and a fixed byte allowance would be
    noise on the large ones and unusable on the small ones. Zero is compared exactly: a file whose
    size is zero cannot differ by a ratio, and that case should fail rather than divide.
    """
    if recorded == measured:
        return True
    if recorded <= 0.0 or measured <= 0.0:
        return False
    larger = max(recorded, measured)
    return abs(recorded - measured) / larger <= BREAKDOWN_ENCODER_SLACK


def breakdown_drift(entries: Dict[str, Dict], measurements: Dict[str, float]) -> List[str]:
    """Errors describing a `recorded` block that no longer matches the measurement.

    The breakdown is what makes a growth *attributable*, so it is checked alongside the total: a file
    whose size moved while the block was not re-recorded means the reviewer reading the diff is told
    the wrong file grew. This is not a second ratchet on the same number -- it does not care what the
    total is, only that the attribution agrees with it. A row the measurement produces but the file
    does not name is an error too: an unrecorded file is an unattributed one, which is exactly the
    state the module coverage in issue #395 exists to end.
    """
    expected = recorded_breakdown(entries, measurements)
    errors: List[str] = []
    for name in sorted(entries):
        recorded = entries[name].get("recorded")
        if not isinstance(recorded, dict):
            continue
        for key in sorted(set(recorded) | set(expected)):
            if key not in recorded:
                errors.append(
                    "{}: the recorded breakdown does not name {}, which the measurement weighs "
                    "({} bytes); an unrecorded file is an unattributed one, so re-record it with "
                    "--update".format(name, key, "{:g}".format(expected[key]))
                )
            elif key not in expected:
                errors.append(
                    "{}: the recorded breakdown names {}, which the measurement no longer weighs "
                    "({} bytes); re-record it with --update".format(
                        name, key, "{:g}".format(float(recorded[key]))
                    )
                )
            elif not same_measurement(float(recorded[key]), expected[key]):
                errors.append(
                    "{}: the recorded breakdown says {} is {} bytes but the measurement says {}; "
                    "re-record it with --update so the attribution matches the total".format(
                        name,
                        key,
                        "{:g}".format(float(recorded[key])),
                        "{:g}".format(expected[key]),
                    )
                )
    return errors


def format_rows(measurements: Dict[str, float]) -> List[str]:
    """The report table: deterministic, sorted, one row per measured name."""
    lines: List[str] = []
    for name in sorted(measurements):
        value = float(measurements[name])
        if value.is_integer():
            lines.append("  {:<34} {:>12}".format(name, int(value)))
        else:
            lines.append("  {:<34} {:>12.3f}".format(name, value))
    return lines


def in_scope(entries: Dict[str, Dict], skipped: Sequence[str]) -> Dict[str, Dict]:
    """The entries a run actually measured, so a size-only run does not report latency as missing."""
    return {name: entry for name, entry in entries.items() if name not in skipped}
