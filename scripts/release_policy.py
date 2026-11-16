#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Findings and the evaluation entry point for the release-manifest gate (issue #193).

This module owns the vocabulary the rest of the policy shares -- :class:`Finding`, the rule table
and :func:`evaluate` -- and delegates each document to its own module:

* :mod:`release_channel_rules` -- ``release/channels.json`` (which channels exist, who may move
  between them, the host allowlist, the pinned keys),
* :mod:`release_ledger_rules` -- ``release/ledger.json`` (what was published, with digests, symbol
  artifacts and deprecation windows),
* :mod:`release_manifest_rules` -- the committed wire manifests (projection, drift, shape, URLs),
* :mod:`release_client` -- the update *policy* seam (``plan_update``), no transport.

Every function returns findings instead of raising, so one run names every problem in the tree. The
output is sorted by path and message: two runs print identical bytes, with no clock, no network and
no environment read anywhere.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional

from release_channel_rules import check_channels
from release_ledger_rules import check_ledger
from release_manifest import project_manifests, render
from release_manifest_rules import check_manifests

PROJECT_VERSION_RE = re.compile(
    r"project\s*\(\s*Tessera\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", re.IGNORECASE
)

# Rule ids, so a failure names the statement it broke instead of a line number that moves.
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
    "manifests.shape": "a manifest carries the fields of spec section 2",
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


def project_version_from_cmake(cmake_text: str) -> Optional[str]:
    match = PROJECT_VERSION_RE.search(cmake_text)
    return match.group(1) if match else None


def has_errors(findings: Iterable[Finding]) -> bool:
    return any(finding.severity == "error" for finding in findings)


def evaluate_data(
    channels: Any,
    ledger: Any,
    project_version: Optional[str] = None,
    channels_path: str = "release/channels.json",
    ledger_path: str = "release/ledger.json",
) -> List[Finding]:
    """Channel and ledger findings only -- the data a projection may be built from."""
    findings = check_channels(channels, channels_path)
    findings.extend(check_ledger(ledger, channels, project_version, ledger_path))
    return sorted(findings)


def evaluate(
    channels: Any,
    ledger: Any,
    committed_manifests: Dict[str, str],
    project_version: Optional[str] = None,
    channels_path: str = "release/channels.json",
    ledger_path: str = "release/ledger.json",
    manifests_path: str = "release/manifests",
) -> List[Finding]:
    """Every finding for a tree, sorted. A caller prints them; the CLI decides the exit code."""
    findings = evaluate_data(channels, ledger, project_version, channels_path, ledger_path)
    if isinstance(channels, dict) and isinstance(ledger, dict):
        projection = project_manifests(ledger, channels)
        findings.extend(
            check_manifests(committed_manifests, projection, ledger, channels, manifests_path)
        )
    return sorted(findings)


def validate_projection(
    channels: Dict[str, Any],
    ledger: Dict[str, Any],
    manifests_path: str = "release/manifests",
) -> List[Finding]:
    """Check the projection *as if it were committed*, before it is written.

    This is what stops the generator from committing an artifact the gate would reject: the shapes
    and URL rules run against the derived documents themselves, so a bad host in the ledger is
    refused by the writer instead of landing as a manifest.
    """
    projection = project_manifests(ledger, channels)
    committed = {name: render(document) for name, document in projection.items()}
    return sorted(check_manifests(committed, projection, ledger, channels, manifests_path))
