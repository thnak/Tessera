#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deprecation-window rules for the release ledger (issue #193).

[`versioning-v1.md`](../../docs/specs/versioning-v1.md) section 11.5 and ADR-049 item 5 require a
deprecated entity to survive **at least two minor releases and ninety days, whichever is longer**,
counted from the release that first deprecated it. This module is that rule.

Both floors need different data, which is why the check runs after the whole ledger is read:

* the **minor** side compares `removedIn` with `deprecatedIn` by SemVer precedence, so no ledger
  ordering is assumed;
* the **date** side compares the *removing* release's recorded date with `deprecatedDate + 90 days`.
  That is only knowable once the removing release is a ledger record, so a `removedIn` naming a
  version the ledger does not have yet is not a failure -- it is simply not checkable, and the
  manifest publishes the computed floor so the constraint is visible in the meantime.

Nothing reads the clock. `deprecatedDate` is data and the removal date is a ledger record, so a test
proves both sides without waiting ninety days or mocking a calendar.
"""

from __future__ import annotations

from typing import Any, Dict, List

from release_findings import Finding, error
from release_manifest import deprecation_window
from release_semver import SemVer, minor_distance, valid_date

# Kept as a module constant so a reader sees the rule id next to the rule that raises it.
WINDOW_RULE = "ledger.deprecation-window"
ENTRY_RULE = "ledger.deprecations"


def check_deprecation(
    label: str, channel_id: str, entry: Any, removal_dates: Dict[str, Dict[str, str]]
) -> List[Finding]:
    """Findings for one deprecation entry of the release at ``label``."""
    where = "{}.deprecations".format(label)
    if not isinstance(entry, dict):
        return [error(where, ENTRY_RULE, "a deprecation entry is not an object")]
    entity = str(entry.get("entity", ""))
    if not entity:
        return [error(where, ENTRY_RULE, "a deprecation entry has no entity")]
    deprecated_in = SemVer.parse(entry.get("deprecatedIn"))
    removed_in = SemVer.parse(entry.get("removedIn"))
    deprecated_date = str(entry.get("deprecatedDate", ""))
    if deprecated_in is None:
        return [
            error(
                where,
                ENTRY_RULE,
                "{!r}: deprecatedIn {!r} is not SemVer".format(entity, entry.get("deprecatedIn")),
            )
        ]
    if removed_in is None:
        return [
            error(
                where,
                ENTRY_RULE,
                "{!r}: removedIn {!r} is not SemVer".format(entity, entry.get("removedIn")),
            )
        ]
    if not valid_date(deprecated_date):
        return [
            error(
                where,
                ENTRY_RULE,
                "{!r}: deprecatedDate {!r} is not YYYY-MM-DD".format(entity, deprecated_date),
            )
        ]
    if removed_in.compare(deprecated_in) <= 0:
        return [
            error(
                where,
                ENTRY_RULE,
                "{!r}: removedIn {} must be after deprecatedIn {}".format(
                    entity, removed_in, deprecated_in
                ),
            )
        ]
    return check_window(where, channel_id, entity, deprecated_in, removed_in, deprecated_date, removal_dates)


def check_window(
    where: str,
    channel_id: str,
    entity: str,
    deprecated_in: SemVer,
    removed_in: SemVer,
    deprecated_date: str,
    removal_dates: Dict[str, Dict[str, str]],
) -> List[Finding]:
    findings: List[Finding] = []
    version_floor, date_floor = deprecation_window(deprecated_in, deprecated_date)
    distance = minor_distance(deprecated_in, removed_in)
    if distance < 2:
        findings.append(
            error(
                where,
                WINDOW_RULE,
                "{!r}: removedIn {} is {} minor release(s) after {}; the window is two minors AND "
                "ninety days, so the earliest legal removal is {}".format(
                    entity, removed_in, max(distance, 0), deprecated_in, version_floor
                ),
            )
        )
    # The ninety-day floor is checkable only once the removing release has a recorded date.
    shipped = removal_dates.get(channel_id, {}).get(str(removed_in))
    if shipped is not None and shipped < date_floor:
        findings.append(
            error(
                where,
                WINDOW_RULE,
                "{!r}: removal shipped on {}, before the ninety-day floor {} (deprecated {})".format(
                    entity, shipped, date_floor, deprecated_date
                ),
            )
        )
    return findings
