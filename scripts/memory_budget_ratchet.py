#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ratchet policy half of the memory budget gate (issue #13).

Pure functions over numbers: no filesystem, no subprocess, no clock. `check_memory_budget.py` is the
CLI that reads the budget file, calls `memory_budget_measure.py` for the numbers, and applies the
policy here. This is the same split the preview budget uses (`preview_budget_ratchet.py`) and for the
same reason: the arithmetic can be unit-tested directly instead of only through a subprocess.

## The two-sided ratchet

Each measured entry is pinned to a number and a pair of tolerances, because the two sides are not
the same kind of claim. With `lower = budget * (1 - better/100)` and
`upper = budget * (1 + worse/100)`:

* `measured > upper`  -> **regressed**: the budget was exceeded by more than the allowed slack,
* `measured < lower`  -> **stale**: the code got much better and nobody recorded it. Slack is how a
  budget rots into a no-op one "small win" at a time,
* otherwise           -> **ok**.

`better` must stay under 100 so the lower bound stays positive; at exactly 100 the staleness side
would be dead and the ratchet would only have one side.

## Three statuses, and why the third one exists

`status` is per entry, and this gate needs one more value than the preview budget did.

* **`enforce`** -- deterministic rows. `sizeof` of a hot type is an ABI constant for a given
  toolchain; an allocation *count* from a replaced global `operator new` in one process is exact.
  Two runs are byte-identical, so the 0%/0% band is a real invariant and a failure is a real
  regression. These are the rows that block a build.
* **`warn`** -- sampled rows (resident bytes, per-node resident cost). These come from the OS: they
  move with page granularity, allocator behaviour, and whatever else the machine is doing, and the
  same binary on the same box varies by megabytes. A gate that fails at random gets deleted, so
  these report the trend on every run inside a generous band and never block. They are still
  measured on every run, which is the part that catches a trend early.
* **`unmeasured`** -- a row that is declared because it belongs in the budget, and that this
  repository's lanes *cannot* measure: Android PSS needs an Android sample that does not exist, and
  the GPU category split needs a Vulkan allocator while every measurement here runs on the null
  backend, which reports zero by design. An `unmeasured` entry has **no `budget`** and a required
  `reason`. That is the whole point: a plausible-looking number for a quantity nobody measured reads
  as evidence and ratchets a fiction, so the validator rejects a budget on an unmeasured entry.
  `unmeasured` is never a pass for something that *could* be measured -- `check_memory_budget.py`
  refuses to run at all if a measured row has no number.

## `--update` refuses to loosen

`--update` re-pins from the current measurements and **refuses to raise any budget**: one entry the
update would have to raise fails the whole update and nothing is written, exactly like
`scripts/check_preview_budget.py` and `scripts/check_source_size.py --update`. The refusal is
uniform across `warn` and `enforce`. A warned RSS row re-pinned on a loaded box would record the
machine's load as the budget, which is rot arriving through the tool rather than through a
regression. Raising a number -- warned or enforced -- is an explicit edit of the budget file in a
reviewed change, with the why in the diff.
"""

from __future__ import annotations

import copy
from typing import Dict, List, Sequence, Tuple

STATUSES = ("warn", "enforce", "unmeasured")

# Fields every entry must carry, whatever its status.
COMMON_FIELDS = ("status", "unit", "measures")


class UsageError(Exception):
    """The budget document does not satisfy the gate's contract."""


def tolerance_for(name: str, tolerance: Dict) -> Tuple[float, float]:
    """`(better_percent, worse_percent)` for one entry.

    A pair, not one number: the two sides answer different questions, and a single symmetric band
    wide enough for a sampled RSS row would push the better side below zero and disable staleness
    detection entirely.
    """
    value = tolerance.get(name, tolerance["default"])
    if isinstance(value, dict):
        return float(value["better"]), float(value["worse"])
    # A bare number is a symmetric band; accepted so a zero-tolerance row stays readable.
    return float(value), float(value)


def bounds_of(budget: float, better_percent: float, worse_percent: float) -> Tuple[float, float]:
    """The accepted band `(lower, upper)`. Lower is the stale side, upper the regression side."""
    return budget * (1.0 - better_percent / 100.0), budget * (1.0 + worse_percent / 100.0)


def _check_amount(label: str, value: object) -> None:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
        raise UsageError("{} must be a non-negative number".format(label))


def _check_percent(label: str, value: object) -> None:
    _check_amount(label, value)
    # Only the *better* side is capped, and it is capped so the lower bound stays positive: at 100%
    # the lower bound is 0 and no entry could ever be stale, which would leave the ratchet with one
    # side. The worse side may be a multiple (a sampled row needs a generous allowance), so capping
    # it the same way would forbid the only band a noisy row can use.
    if float(value) >= 100.0:
        raise UsageError(
            "{0} must be under 100 (at 100 the lower bound is 0 and no budget could ever be "
            "stale)".format(label)
        )


def validate_tolerance(tolerance: object) -> Dict:
    """Checks the `tolerance` block: a default, and every value a non-negative pair or number."""
    if not isinstance(tolerance, dict) or "default" not in tolerance:
        raise UsageError("the budget file has no `tolerance.default`")

    def check(label: str, value: object) -> None:
        if isinstance(value, dict):
            for side in ("better", "worse"):
                if side not in value:
                    raise UsageError("{}.{} has no {!r}".format(label, side, side))
            _check_percent("{}.better".format(label), value["better"])
            _check_amount("{}.worse".format(label), value["worse"])
            return
        _check_percent(label, value)

    check("tolerance.default", tolerance["default"])
    for key, value in sorted(tolerance.items()):
        if key != "default":
            check("tolerance.{}".format(key), value)
    return tolerance


def _validate_entry(name: str, entry: object) -> None:
    if not isinstance(entry, dict):
        raise UsageError("entry {} is not an object".format(name))
    for field in COMMON_FIELDS:
        if field not in entry:
            raise UsageError("entry {} has no {!r}".format(name, field))
    status = entry["status"]
    if status not in STATUSES:
        raise UsageError(
            "entry {} has status {!r}; expected one of {}".format(name, status, STATUSES)
        )

    if status == "unmeasured":
        # A declared-but-unmeasurable row must say why and must not carry a number: a budget here
        # would read as evidence for a quantity nobody measured, which is the failure this status
        # exists to prevent.
        if "budget" in entry:
            raise UsageError(
                "entry {} is `unmeasured` but carries a `budget`; a number for a quantity this "
                "lane cannot measure is an invention -- record the reason instead".format(name)
            )
        reason = entry.get("reason")
        if not isinstance(reason, str) or not reason.strip():
            raise UsageError("entry {} is `unmeasured` but has no `reason`".format(name))
        return

    if "budget" not in entry:
        raise UsageError("entry {} has no 'budget'".format(name))
    budget = entry["budget"]
    if isinstance(budget, bool) or not isinstance(budget, (int, float)):
        raise UsageError("entry {} budget must be a number".format(name))
    # A 0 budget is valid and meaningful for an *exact-count* row: "this allocates nothing" is a
    # real invariant (a cleared table must not reallocate, an idle cycle must not grow), and it is
    # the strongest form of the claim rather than a typo. Byte budgets are ceilings and a 0 byte
    # ceiling is meaningless, so they still have to be positive.
    if budget < 0:
        raise UsageError("entry {} budget must not be negative".format(name))
    if status == "enforce" and budget <= 0 and entry.get("unit") != "allocations":
        raise UsageError(
            "entry {} is enforced with a non-positive byte budget; only an `allocations` row can "
            "legitimately be 0".format(name)
        )


def validate(document: object) -> Dict:
    """Checks the budget document's shape and returns it, or raises `UsageError`.

    Strict and up front: a typo in a key would otherwise silently drop an entry from the comparison,
    which is the same failure mode as a gate that does not run at all.
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
        _validate_entry(name, entry)
    return document


def amount(value: float, unit: str) -> str:
    """A reading with its unit. Byte counts are exact integers; a percentage-scaled bound is not."""
    if float(value).is_integer():
        return "{:.0f} {}".format(value, unit)
    return "{:.3f} {}".format(value, unit)


def classified(entries: Dict[str, Dict]) -> Tuple[Dict[str, Dict], Dict[str, Dict]]:
    """`(measured, unmeasured)` in the entry table, so both halves can be reported."""
    measured = {name: entry for name, entry in entries.items() if entry["status"] != "unmeasured"}
    unmeasured = {name: entry for name, entry in entries.items() if entry["status"] == "unmeasured"}
    return measured, unmeasured


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

    `unmeasured` entries are out of scope by construction: they are reported by `unmeasured_notes`
    instead, because a row this lane cannot measure is neither a pass nor a failure. A measured
    entry without a number IS an error: a budget nobody measured is not a budget that passed.
    """
    errors: List[str] = []
    warnings: List[str] = []
    notes: List[str] = []
    for name in sorted(entries):
        entry = entries[name]
        if entry["status"] == "unmeasured":
            continue
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


def unmeasured_notes(entries: Dict[str, Dict], measurements: Dict[str, float]) -> List[str]:
    """One line per declared-but-unmeasured row, with its reason.

    Printed on every run so the gap is visible instead of being remembered by someone. If a row that
    was unmeasurable starts reporting a number (a Vulkan lane, an Android sample), it is reported
    here as promotable rather than silently compared against a budget it does not have.
    """
    notes: List[str] = []
    for name in sorted(entries):
        entry = entries[name]
        if entry["status"] != "unmeasured":
            continue
        if name in measurements:
            notes.append(
                "{}: now measured ({} {}) -- promote it to a warn/enforce row with a budget".format(
                    name, amount(float(measurements[name]), entry.get("unit", "")), entry.get("unit", "")
                )
            )
        else:
            notes.append("{}: declared, not measured -- {}".format(name, entry["reason"]))
    return notes


def refused_by_update(entries: Dict[str, Dict], measurements: Dict[str, float]) -> List[str]:
    """The entries an `--update` would have to loosen, so it must write nothing.

    Uniform across `warn` and `enforce`, for the reason in the module docstring: a warned RSS row
    re-pinned on a loaded box records the machine's load as the budget. Raising any number is a
    reviewed edit of the budget file instead.
    """
    refused: List[str] = []
    for name in sorted(entries):
        entry = entries[name]
        if entry["status"] == "unmeasured" or name not in measurements:
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


def update_notes(entries: Dict[str, Dict], measurements: Dict[str, float]) -> List[str]:
    """What an `--update` reports: the entries it tightened, and by how much.

    Only tightenings appear, because `refused_by_update` has already vetoed the rest. They are
    printed rather than silent so a reviewer sees a budget move in the CI log, not only in the diff.
    """
    notes: List[str] = []
    for name in sorted(entries):
        entry = entries[name]
        if entry["status"] == "unmeasured" or name not in measurements:
            continue
        pinned = float(entry["budget"])
        measured = round(float(measurements[name]), 3)
        if measured < pinned:
            notes.append(
                "{}: tightened from {} to {} {}".format(
                    name, "{:g}".format(pinned), "{:g}".format(measured), entry.get("unit", "")
                )
            )
    return notes


def repinned(document: Dict, measurements: Dict[str, float], counts: Dict, build_config: str = "") -> Dict:
    """The budget document after `--update`: pinned numbers and the sample counts refreshed."""
    updated = copy.deepcopy(document)
    updated["measurement"] = counts
    if build_config:
        # The configuration is part of the claim: the same source built Debug and Release gives
        # different sizeof(Node) and allocation counts, so the file records which one it pinned.
        updated["measurement"]["build_config"] = build_config
    for name, entry in updated["entries"].items():
        if entry["status"] == "unmeasured" or name not in measurements:
            continue
        value = float(measurements[name])
        # Integer rows stay integers: they are byte counts and counts, not scaled decimals.
        entry["budget"] = int(value) if value.is_integer() else round(value, 3)
    return updated


def format_rows(measurements: Dict[str, float]) -> List[str]:
    """The report table: deterministic, sorted, one row per measured name."""
    lines: List[str] = []
    for name in sorted(measurements):
        value = float(measurements[name])
        if value.is_integer():
            lines.append("  {:<30} {:>12}".format(name, int(value)))
        else:
            lines.append("  {:<30} {:>12.3f}".format(name, value))
    return lines


def in_scope(entries: Dict[str, Dict], skipped: Sequence[str]) -> Dict[str, Dict]:
    """The entries a run actually measured, so a sizes-only run does not report RSS as missing."""
    return {name: entry for name, entry in entries.items() if name not in skipped}
