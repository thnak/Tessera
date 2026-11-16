#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Findings and the rule ids the distribution gate reports (issue #193).

The vocabulary every other ``release_*`` module shares. Kept in its own module so the rules can
import it without importing each other in a cycle, and so a reader has one table of what the gate
can fail on.

A :class:`Finding` carries a **rule id** rather than a line number: a line number moves on the next
edit, and the rule is the statement the document broke. Findings sort by path and then by message, so
``check_release_manifest.py`` prints identical bytes on two runs over the same tree -- no clock, no
network, no environment read anywhere in this package.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Iterable

# Rule ids. The keys are stable identifiers used in output and in tests; the values are one-line
# statements of what the rule requires.
RULES: Dict[str, str] = {
    "channels.version": "channelsVersion is the accepted document version",
    "channels.id": "channel ids are lower-case, unique and one of stable/beta/nightly",
    "channels.required": "the stable and beta channels must both be defined",
    "channels.rank": "each channel has a distinct rank; stable has the lowest (most stable)",
    "channels.consent": "consent is one of onRestart/prompt/manual",
    "channels.transition": "every ordered pair of channels has exactly one transition row",
    "channels.transition-direction": "moving to a less stable channel is optIn",
    "channels.transition-stable": "moving to a more stable channel is free",
    "channels.downgrade": "the downgrade policy is blocked or signed",
    "channels.hosts": "updateHosts are bare lower-case hosts, not URLs",
    "channels.signing": "signing is Ed25519 with pinned keys; an empty list is the unsigned state",
    "ledger.version": "ledgerVersion is the accepted document version",
    "ledger.release": "a release record has version/channel/date/commit/artifacts",
    "ledger.unique": "a (channel, version) pair is recorded once",
    "ledger.channel": "a release names a channel defined in channels.json",
    "ledger.artifact": "artifact platform/name/sha256/size are the shipped archive's",
    "ledger.symbols": "every artifact declares a tessera.symbols.v1 artifact for its own build",
    "ledger.signature": "a signature is null, or 64 bytes of hex with a pinned key",
    "ledger.min-os": "minOs is a dotted numeric version when present",
    "ledger.prerelease": "a channel marked prerelease:false carries no pre-release version",
    "ledger.project-version": "no recorded release is newer than project(Tessera VERSION ...)",
    "ledger.deprecations": "a deprecation names its entity, the release and the removal release",
    "ledger.deprecation-window": "removal is at least two minors and ninety days after deprecation",
    "manifests.projection": "release/manifests/ is exactly the projection of the ledger",
    "manifests.drift": "each committed manifest is byte-identical to its projection",
    "manifests.shape": "a manifest carries the fields of specification section 2",
    "manifests.url": "update/notes URLs are https on an allowlisted host",
    "manifests.identity": "a manifest's channel/platform match its file name",
    "manifests.artifact": "a manifest's sha256/size/symbols/deprecations match the ledger",
}


@dataclass(frozen=True, order=True)
class Finding:
    """One policy failure or warning. Ordered by path, then message, so output is deterministic."""

    path: str
    severity: str  # "error" | "warn"
    rule: str
    message: str

    def format(self) -> str:
        prefix = "ERROR" if self.severity == "error" else "WARN"
        return "{} {}: [{}] {}".format(prefix, self.path, self.rule, self.message)


def error(path: str, rule: str, message: str) -> Finding:
    return Finding(path, "error", rule, message)


def warn(path: str, rule: str, message: str) -> Finding:
    return Finding(path, "warn", rule, message)


def has_errors(findings: Iterable[Finding]) -> bool:
    return any(finding.severity == "error" for finding in findings)
