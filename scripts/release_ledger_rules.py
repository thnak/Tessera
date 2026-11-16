#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""``release/ledger.json`` rules (issue #193, specification sections 2, 3 and 6.1).

The ledger is the append-only record of what shipped. It is what lets the tooling answer three
questions the specification asks, without a network and without a clock:

* **Is this a downgrade?** A release's version is compared with the ledger's history on that
  channel by SemVer precedence, never by date.
* **Which exact bytes shipped?** Each artifact records the archive name, size and SHA-256 the
  package gate already validates, plus the *symbol artifact* for the same build. A symbol file whose
  build id is not the release commit is refused, because the resolver would refuse it at crash time
  and a release that cannot symbolicate its own crashes is worse than one that says so
  ([`crash-reporting-v1.md`](../../docs/specs/crash-reporting-v1.md) section 3).
* **May a deprecated entity be removed?** versioning-v1 section 11.5: at least two minor releases
  **and** ninety days, whichever is longer. Both floors are checked; checking only one lets an
  entity be removed early through the other.

Release records also carry the *release date as data*. Nothing in this module reads the clock, which
is why the ninety-day floor is a computable rule rather than a note.
"""

from __future__ import annotations

import re
from typing import Any, Dict, List, Optional, Tuple

from release_data_rules import signature_is_hex
from release_deprecation_rules import check_deprecation
from release_findings import Finding, error, warn
from release_manifest import (
    LEDGER_VERSION,
    PLATFORMS,
    SHA256_RE,
    SYMBOLS_FORMAT,
    expected_archive_name,
    expected_symbols_name,
    platform_suffix,
    signing_keys,
)
from release_semver import SemVer, valid_date

COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
MIN_OS_RE = re.compile(r"^[0-9]+(?:\.[0-9]+)*$")
SIGNATURE_HEX_LENGTH = 128  # Ed25519: 64 bytes


def check_ledger(
    ledger: Any,
    channels: Any,
    project_version: Optional[str] = None,
    path: str = "release/ledger.json",
) -> List[Finding]:
    findings: List[Finding] = []
    if not isinstance(ledger, dict):
        return [error(path, "ledger.release", "the ledger must be a JSON object")]
    if ledger.get("ledgerVersion") != LEDGER_VERSION:
        findings.append(
            error(
                path,
                "ledger.version",
                "ledgerVersion must be {} (found {!r})".format(
                    LEDGER_VERSION, ledger.get("ledgerVersion")
                ),
            )
        )
    releases = ledger.get("releases")
    if not isinstance(releases, list):
        return findings + [
            error(path, "ledger.release", "releases must be an array (possibly empty)")
        ]

    declared = _declared_channels(channels)
    pinned_keys = signing_keys(channels) if isinstance(channels, dict) else []
    newest: Dict[str, SemVer] = {}
    removal_dates: Dict[str, Dict[str, str]] = {}
    seen_pairs: set = set()
    deprecations: List[Tuple[str, Dict[str, Any]]] = []

    for index, release in enumerate(releases):
        label = "{}[{}]".format(path, index)
        if not isinstance(release, dict):
            findings.append(
                error(path, "ledger.release", "releases[{}] is not an object".format(index))
            )
            continue
        version = SemVer.parse(release.get("version"))
        if version is None:
            findings.append(
                error(
                    label,
                    "ledger.release",
                    "version {!r} is not SemVer".format(release.get("version")),
                )
            )
            continue
        channel_id = str(release.get("channel", ""))
        findings.extend(_check_release_header(label, release, version, channel_id, declared))
        findings.extend(_check_release_signature(label, release, pinned_keys))
        findings.extend(_check_artifacts(label, release, version))

        pair = (channel_id, str(version))
        if pair in seen_pairs:
            findings.append(
                error(
                    label,
                    "ledger.unique",
                    "channel {!r} already records version {}; a release is recorded once".format(
                        channel_id, version
                    ),
                )
            )
        seen_pairs.add(pair)
        _record_newest(newest, removal_dates, channel_id, version, str(release.get("date", "")))

        deprecations.extend((label, channel_id, entry) for entry in (release.get("deprecations") or []))

    # Deprecations are checked after every release is known, because the ninety-day floor can only
    # be applied once the *removing* release's date is on record.
    for label, channel_id, entry in deprecations:
        findings.extend(check_deprecation(label, channel_id, entry, removal_dates))

    findings.extend(_check_project_version(path, newest, project_version, declared))
    return findings


def _declared_channels(channels: Any) -> Dict[str, Dict[str, Any]]:
    if not isinstance(channels, dict):
        return {}
    return {
        str(channel.get("id")): channel
        for channel in (channels.get("channels") or [])
        if isinstance(channel, dict)
    }


def _record_newest(
    newest: Dict[str, SemVer],
    removal_dates: Dict[str, Dict[str, str]],
    channel_id: str,
    version: SemVer,
    day: str,
) -> None:
    removal_dates.setdefault(channel_id, {})[str(version)] = day
    current = newest.get(channel_id)
    if current is None or version.compare(current) > 0:
        newest[channel_id] = version


def _check_release_header(
    label: str,
    release: Dict[str, Any],
    version: SemVer,
    channel_id: str,
    declared: Dict[str, Dict[str, Any]],
) -> List[Finding]:
    findings: List[Finding] = []
    if channel_id not in declared:
        findings.append(
            error(
                label,
                "ledger.channel",
                "channel {!r} is not defined in channels.json".format(channel_id),
            )
        )
    elif declared[channel_id].get("prerelease") is False and version.prerelease:
        findings.append(
            error(
                label,
                "ledger.prerelease",
                "{} is a pre-release but channel {!r} does not accept one".format(
                    version, channel_id
                ),
            )
        )
    if not valid_date(release.get("date")):
        findings.append(
            error(
                label,
                "ledger.release",
                "date {!r} is not YYYY-MM-DD".format(release.get("date")),
            )
        )
    commit = str(release.get("commit", ""))
    if COMMIT_RE.match(commit) is None:
        findings.append(
            error(label, "ledger.release", "commit {!r} is not a 40-hex commit".format(commit))
        )
    min_os = release.get("minOs")
    if min_os is not None and MIN_OS_RE.match(str(min_os)) is None:
        findings.append(
            error(label, "ledger.min-os", "minOs {!r} is not a dotted numeric version".format(min_os))
        )
    return findings


def _check_release_signature(
    label: str, release: Dict[str, Any], pinned_keys: List[Dict[str, Any]]
) -> List[Finding]:
    signature = release.get("signature")
    if signature is None:
        return []
    findings: List[Finding] = []
    if not signature_is_hex(signature, SIGNATURE_HEX_LENGTH):
        findings.append(
            error(
                label,
                "ledger.signature",
                "signature {!r} is not {} lower-case hex characters".format(
                    signature, SIGNATURE_HEX_LENGTH
                ),
            )
        )
    if not pinned_keys:
        findings.append(
            error(
                label,
                "ledger.signature",
                "a signature is recorded but channels.json pins no key, so nothing could have "
                "signed or verified it",
            )
        )
    return findings


def _check_artifacts(label: str, release: Dict[str, Any], version: SemVer) -> List[Finding]:
    findings: List[Finding] = []
    artifacts = release.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        return [
            error(
                "{}.artifacts".format(label),
                "ledger.release",
                "a release records at least one platform artifact",
            )
        ]
    seen_platforms = set()
    for index, artifact in enumerate(artifacts):
        where = "{}.artifacts[{}]".format(label, index)
        if not isinstance(artifact, dict):
            findings.append(error(where, "ledger.artifact", "artifact is not an object"))
            continue
        platform = str(artifact.get("platform", ""))
        if platform not in PLATFORMS:
            findings.append(
                error(
                    where,
                    "ledger.artifact",
                    "platform {!r} is not one of {}".format(platform, ", ".join(PLATFORMS)),
                )
            )
            continue
        if platform in seen_platforms:
            findings.append(
                error(
                    where,
                    "ledger.artifact",
                    "platform {!r} appears twice in one release".format(platform),
                )
            )
        seen_platforms.add(platform)
        findings.extend(_check_one_artifact(where, artifact, version, platform, release))
    return findings


def _check_one_artifact(
    where: str,
    artifact: Dict[str, Any],
    version: SemVer,
    platform: str,
    release: Dict[str, Any],
) -> List[Finding]:
    findings: List[Finding] = []
    expected = expected_archive_name(str(version), platform, platform_suffix(platform))
    archive = str(artifact.get("archive", ""))
    if archive != expected:
        findings.append(
            error(
                where,
                "ledger.artifact",
                "archive is {!r}; the package layout names it {!r}".format(archive, expected),
            )
        )
    if SHA256_RE.match(str(artifact.get("sha256", ""))) is None:
        findings.append(error(where, "ledger.artifact", "sha256 is not 64 lower-case hex"))
    size = artifact.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        findings.append(error(where, "ledger.artifact", "size must be a positive integer"))
    findings.extend(_check_symbols(where, artifact.get("symbols"), version, platform, release))
    return findings


def _check_symbols(
    where: str,
    symbols: Any,
    version: SemVer,
    platform: str,
    release: Dict[str, Any],
) -> List[Finding]:
    findings: List[Finding] = []
    if not isinstance(symbols, dict):
        return [
            error(
                where,
                "ledger.symbols",
                "no symbol artifact recorded; a release without symbols cannot symbolicate its "
                "own crash reports (crash-reporting-v1 section 3)",
            )
        ]
    expected = expected_symbols_name(str(version), platform)
    if str(symbols.get("artifact", "")) != expected:
        findings.append(
            error(
                where,
                "ledger.symbols",
                "symbol artifact is {!r}; expected {!r}".format(symbols.get("artifact"), expected),
            )
        )
    if SHA256_RE.match(str(symbols.get("sha256", ""))) is None:
        findings.append(error(where, "ledger.symbols", "symbol sha256 is not 64 lower-case hex"))
    size = symbols.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        findings.append(error(where, "ledger.symbols", "symbol size must be a positive integer"))
    if str(symbols.get("format", "")) != SYMBOLS_FORMAT:
        findings.append(
            error(
                where,
                "ledger.symbols",
                "symbol format is {!r}; expected {!r}".format(symbols.get("format"), SYMBOLS_FORMAT),
            )
        )
    build_id = str(symbols.get("build", ""))
    if COMMIT_RE.match(build_id) is None:
        findings.append(error(where, "ledger.symbols", "symbol build id is not a 40-hex commit"))
    elif build_id != str(release.get("commit", "")):
        findings.append(
            error(
                where,
                "ledger.symbols",
                "symbol build id {!r} is not the release commit; the resolver refuses a "
                "mismatch instead of naming frames wrongly".format(build_id),
            )
        )
    return findings


def _check_project_version(
    path: str,
    newest: Dict[str, SemVer],
    project_version: Optional[str],
    declared: Dict[str, Dict[str, Any]],
) -> List[Finding]:
    if project_version is None:
        return []
    project = SemVer.parse(project_version)
    if project is None:
        return [
            error(
                path,
                "ledger.project-version",
                "project version {!r} is not SemVer".format(project_version),
            )
        ]
    findings: List[Finding] = []
    for channel_id, version in sorted(newest.items()):
        core = SemVer(version.major, version.minor, version.patch)
        if core.compare(project) > 0:
            findings.append(
                error(
                    path,
                    "ledger.project-version",
                    "channel {!r} records {} but project(Tessera VERSION {}) is older; a release "
                    "bumps the version it ships".format(channel_id, version, project_version),
                )
            )
    stable = newest.get("stable")
    if stable is not None and stable.compare(project) < 0 and "stable" in declared:
        findings.append(
            warn(
                path,
                "ledger.project-version",
                "the newest stable release is {} while the tree is {}".format(stable, project_version),
            )
        )
    return findings
