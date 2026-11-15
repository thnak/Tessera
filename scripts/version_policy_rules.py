#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The versioning policy itself, as rules over a recorded surface (issue #194).

:mod:`check_version_policy` is the command-line gate; this module is the reasoning it applies, kept
apart so the policy can be read and tested without the argument parsing and file writing around it.

One :class:`Finding` per problem. ``kind`` is data, not message text, because ``--update`` keys on it:
``unversioned-breaking`` is the failure the policy exists for and no recording may launder it,
``regression`` is a version that went backwards, and ``unrecorded`` means the change may well be
correct but the baseline does not describe the tree yet.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

import version_baseline as baseline  # noqa: E402  (sibling script, stdlib only)
import version_surface as surface  # noqa: E402  (sibling script, stdlib only)

SURFACE_ORDER: Tuple[str, ...] = ("ir", "rhi", "tessera.json")

SURFACE_LABEL: Dict[str, str] = {
    "ir": "IR schema",
    "rhi": "RHI source contract",
    "tessera.json": "`tessera.json` schema",
}
SURFACE_VERSION_SITE: Dict[str, str] = {
    "ir": "kIrVersion in " + surface.IR_VERSION_HEADER,
    "rhi": "kRhiVersionMajor/kRhiVersionMinor in " + surface.RHI_VERSION_HEADER,
    "tessera.json": "the accepted `schemaVersion` in " + surface.TESSERA_JSON_LOADER,
}
SURFACE_POLICY: Dict[str, str] = {
    "ir": "docs/specs/versioning-v1.md section 2",
    "rhi": "docs/specs/versioning-v1.md section 4 and ADR-049",
    "tessera.json": "docs/specs/versioning-v1.md section 2",
}

UNRECORDABLE = ("unversioned-breaking", "regression")


def describe_version(value: object) -> str:
    if isinstance(value, list):
        return ".".join(str(part) for part in value)
    return str(value)


def version_increased(old: object, new: object) -> bool:
    """True when ``new`` is strictly greater than ``old`` (a pair compares lexicographically)."""
    if isinstance(old, list) and isinstance(new, list):
        return tuple(new) > tuple(old)
    if isinstance(old, int) and isinstance(new, int):
        return new > old
    return new != old


def classify_fields(
    old: Dict[str, Dict[str, str]], new: Dict[str, Dict[str, str]]
) -> Tuple[List[str], List[str]]:
    """Return ``(breaking, additive)`` descriptions for one schema surface.

    **Breaking** is what the policy says must move the version: a removed field, a changed type (a
    narrowed or swapped type), a field that became required, a changed default (the artifact now
    means something different), or a new *required* field (a producer that omits it now fails).
    **Additive** is a new optional field, which the policy exempts from a bump.
    """
    breaking: List[str] = []
    additive: List[str] = []
    for key in sorted(set(old) | set(new)):
        if key not in new:
            breaking.append(
                "removed `{}` (declared in {})".format(key, surface.source_of(key))
            )
            continue
        entry = new[key]
        if key not in old:
            if entry.get("required") == "yes":
                breaking.append(
                    "added required `{}` of type `{}` ({})".format(
                        key, entry["type"], entry["source"]
                    )
                )
            else:
                additive.append(
                    "added optional `{}` of type `{}` ({})".format(
                        key, entry["type"], entry["source"]
                    )
                )
            continue
        before = old[key]
        if before.get("type") != entry.get("type"):
            breaking.append(
                "`{}` changed type from `{}` to `{}` ({})".format(
                    key, before.get("type"), entry.get("type"), entry["source"]
                )
            )
        if before.get("required") != entry.get("required"):
            breaking.append(
                "`{}` changed required-ness from `{}` to `{}` ({})".format(
                    key, before.get("required", "-"), entry.get("required", "-"), entry["source"]
                )
            )
        if before.get("default") != entry.get("default"):
            breaking.append(
                "`{}` changed default from `{}` to `{}` ({})".format(
                    key, before.get("default", "-"), entry.get("default", "-"), entry["source"]
                )
            )
    return breaking, additive


@dataclass(frozen=True)
class Finding:
    """One gate result.

    ``kind`` is what ``--update`` keys on: ``unversioned-breaking`` is the failure the gate exists
    for and is never recordable; ``unrecorded`` means the change may be fine but the baseline does
    not describe the tree; ``regression`` means the version went backwards, which no recording can
    fix. Keeping the kind as data rather than re-parsing message text is what lets ``--update``
    refuse precisely the one case it must never launder.
    """

    kind: str  # "unversioned-breaking" | "unrecorded" | "regression"
    anchor: str
    message: str


UNRECORDABLE = ("unversioned-breaking", "regression")


def evaluate(root: Path) -> Tuple[List[Finding], List[str], Dict[str, Dict[str, object]]]:
    """Return ``(findings, warnings, current surfaces)`` for the tree at ``root``."""
    current = surface.build_surfaces(root)
    recorded = baseline.load_baseline(root)
    findings: List[Finding] = []
    warnings: List[str] = []

    if recorded is None:
        findings.append(
            Finding(
                "unrecorded",
                baseline.BASELINE,
                "{}: no recorded surfaces; run `python -B scripts/check_version_policy.py "
                "--update` to record the reviewed state".format(baseline.BASELINE),
            )
        )
        return findings, warnings, current

    for name in SURFACE_ORDER:
        entry = current[name]
        previous = recorded.get(name)
        anchor = surface.SURFACE_ANCHOR[name]
        if not isinstance(previous, dict):
            findings.append(
                Finding(
                    "unrecorded",
                    baseline.BASELINE,
                    "{}: surface `{}` is not recorded; run `--update`".format(
                        baseline.BASELINE, name
                    ),
                )
            )
            continue
        findings.extend(_surface_findings(name, previous, entry, anchor, warnings))
    return findings, warnings, current


def _surface_findings(
    name: str,
    previous: Dict[str, object],
    entry: Dict[str, object],
    anchor: str,
    warnings: List[str],
) -> List[Finding]:
    """Compare one surface against its recording and classify the difference."""
    label = SURFACE_LABEL[name]
    site = SURFACE_VERSION_SITE[name]
    policy = SURFACE_POLICY[name]
    old = describe_version(previous.get("version"))
    new = describe_version(entry["version"])
    version_moved = entry["version"] != previous.get("version")

    if version_moved and not version_increased(previous.get("version"), entry["version"]):
        return [
            Finding(
                "regression",
                anchor,
                "{}: {} moved from {} to {} without increasing; a version never goes backwards "
                "and a bump increases it (policy: {})".format(anchor, site, old, new, policy),
            )
        ]

    if name == "rhi":
        contract_moved = entry["digest"] != previous.get("digest")
        if contract_moved and not version_moved:
            return [
                Finding(
                    "unversioned-breaking",
                    anchor,
                    "{}: the {} changed but {} is unchanged ({}); bump kRhiVersionMinor for an "
                    "additive change or kRhiVersionMajor for a breaking one (ADR-049), state the "
                    "impact in docs/design/gpu/rhi.md, then record it with --update "
                    "(policy: {})".format(anchor, label, site, old, policy),
                )
            ]
        if contract_moved or version_moved:
            return [
                Finding(
                    "unrecorded",
                    anchor,
                    _record_hint(),
                )
            ]
        return []

    breaking, additive = classify_fields(previous.get("fields") or {}, entry["fields"])
    if breaking and not version_moved:
        return [
            Finding(
                "unversioned-breaking",
                anchor,
                "{}: breaking {} change without a version bump ({} stays {}): {}; bump {} and "
                "record it with --update (policy: {})".format(
                    anchor, label, site, old, "; ".join(breaking), site, policy
                ),
            )
        ]
    if breaking:
        return [
            Finding(
                "unrecorded",
                anchor,
                "{}: breaking {} change ({}): {}; {}".format(
                    anchor,
                    label,
                    _version_note(old, new),
                    "; ".join(breaking),
                    _record_hint(),
                ),
            )
        ]
    if additive and version_moved:
        warnings.append(
            "{}: adding optional {} fields keeps the version (policy: {}), but {} moved {} -> {}; "
            "keep the bump if it carries another change, otherwise revert it".format(
                anchor, label, policy, site, old, new
            )
        )
    if additive:
        return [
            Finding(
                "unrecorded",
                anchor,
                "{}: additive {} change, no version bump needed ({}): {}; {}".format(
                    anchor,
                    label,
                    _version_note(old, new),
                    "; ".join(additive),
                    _record_hint(),
                ),
            )
        ]
    if version_moved:
        return [
            Finding(
                "unrecorded",
                anchor,
                "{}: {} moved {} -> {}; {}".format(
                    anchor, site, old, new, _record_hint()
                ),
            )
        ]
    return []


def _version_note(old: str, new: str) -> str:
    """One phrase for the version in a message that does not assume the version moved."""
    return "version unchanged at {}".format(new) if old == new else "version {} -> {}".format(old, new)


def _record_hint() -> str:
    return (
        "run `python -B scripts/check_version_policy.py --update` and commit "
        "scripts/versioning-baseline.txt with the change"
    )
