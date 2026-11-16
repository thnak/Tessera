#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Committed wire manifests: projection, drift, shape and URLs (issue #193, section 2).

``release/manifests/<channel>-<platform>.json`` is derived from the ledger and committed. That is the
whole ratchet: the file in git must equal the projection, so editing either the ledger or a manifest
without the other is a gate failure, and the diff a reviewer reads *is* the offer a client will see.

What is checked:

* **Projection.** The set of committed files is exactly the set the ledger projects. A missing file
  is an offer that silently disappeared; an extra file is an offer nothing recorded.
* **Drift.** Each committed file is byte-identical to ``json.dumps(projection, indent=2,
  sort_keys=True) + "\\n"``. No timestamp, no path, no ordering freedom.
* **Shape.** The spec section-2 fields are all present, the version parses, and the symbols block
  exists: a release that ships no symbol artifact cannot symbolicate its own crashes.
* **Identity.** ``channel``/``platform`` inside the document match the file name, so a renamed file
  cannot masquerade as a different channel.
* **URLs.** Any ``url``/``notesUrl`` is ``https`` on a host from the channel file's allowlist. A
  ``null`` URL is the honest state while no host is configured, and it is what makes "no network in
  a test" true by construction.
* **The artifact facts** (sha256, size, deprecations) come from the ledger record, not from the
  manifest's own text.
"""

from __future__ import annotations

from typing import Any, Dict, List, Sequence, Tuple

from release_data_rules import signature_is_hex
from release_findings import Finding, error
from release_manifest import (
    MANIFEST_FIELDS,
    MANIFEST_VERSION,
    SHA256_RE,
    SYMBOLS_FORMAT,
    find_artifact,
    manifest_file_name,
    projected_deprecations,
    render,
    signing_keys,
)
from release_semver import SemVer

SIGNATURE_HEX_LENGTH = 128


def check_manifests(
    committed: Dict[str, str],
    projection: Dict[str, Dict[str, Any]],
    ledger: Dict[str, Any],
    channels: Dict[str, Any],
    path: str = "release/manifests",
) -> List[Finding]:
    """``committed`` maps file name -> text; ``projection`` is the derived set."""
    findings: List[Finding] = []
    hosts = [str(host).lower() for host in (channels.get("updateHosts") or [])]
    keys = signing_keys(channels) if isinstance(channels, dict) else []

    expected_names = set(projection)
    committed_names = set(committed)
    for name in sorted(expected_names - committed_names):
        findings.append(
            error(
                "{}/{}".format(path, name),
                "manifests.projection",
                "missing: the ledger projects a manifest here",
            )
        )
    for name in sorted(committed_names - expected_names):
        findings.append(
            error(
                "{}/{}".format(path, name),
                "manifests.projection",
                "stale: the ledger projects no manifest with this name",
            )
        )
    for name in sorted(expected_names & committed_names):
        where = "{}/{}".format(path, name)
        document = projection[name]
        if committed[name] != render(document):
            findings.append(
                error(where, "manifests.drift", "not byte-identical to its projection")
            )
            continue
        findings.extend(_check_manifest_shape(where, document, ledger, hosts, keys))
    return findings


def _check_manifest_shape(
    where: str,
    manifest: Dict[str, Any],
    ledger: Dict[str, Any],
    hosts: Sequence[str],
    keys: Sequence[Dict[str, Any]],
) -> List[Finding]:
    findings: List[Finding] = []
    missing = [field for field in MANIFEST_FIELDS if field not in manifest]
    if missing:
        findings.append(
            error(where, "manifests.shape", "missing field(s): {}".format(", ".join(missing)))
        )
    if manifest.get("manifestVersion") != MANIFEST_VERSION:
        findings.append(
            error(
                where,
                "manifests.shape",
                "manifestVersion is {!r}, expected {}".format(
                    manifest.get("manifestVersion"), MANIFEST_VERSION
                ),
            )
        )
    channel_id, platform = str(manifest.get("channel", "")), str(manifest.get("platform", ""))
    if manifest_file_name(channel_id, platform) != where.rsplit("/", 1)[-1]:
        findings.append(
            error(
                where,
                "manifests.identity",
                "channel/platform do not match the file name ({} / {})".format(channel_id, platform),
            )
        )
    version = SemVer.parse(manifest.get("version"))
    if version is None:
        findings.append(
            error(
                where,
                "manifests.shape",
                "version {!r} is not SemVer".format(manifest.get("version")),
            )
        )
        return findings
    if SHA256_RE.match(str(manifest.get("sha256", ""))) is None:
        findings.append(error(where, "manifests.shape", "sha256 is not 64 lower-case hex"))
    for field in ("url", "notesUrl"):
        value = manifest.get(field)
        if value is not None:
            findings.extend(_check_url(where, field, str(value), hosts))
    signature = manifest.get("signature")
    if signature is not None:
        if not keys:
            findings.append(
                error(
                    where,
                    "manifests.shape",
                    "a signature is offered but no signing key is pinned, so a client could not "
                    "verify it",
                )
            )
        if not signature_is_hex(signature, SIGNATURE_HEX_LENGTH):
            findings.append(
                error(
                    where,
                    "manifests.shape",
                    "signature is not a 64-byte Ed25519 signature in lower-case hex",
                )
            )
    findings.extend(_check_symbols_block(where, manifest.get("symbols"), hosts))
    findings.extend(_check_artifact_facts(where, manifest, ledger, channel_id, platform, version))
    return findings


def _check_symbols_block(where: str, symbols: Any, hosts: Sequence[str]) -> List[Finding]:
    if not isinstance(symbols, dict):
        return [
            error(
                where,
                "manifests.artifact",
                "no symbols block: a client cannot symbolicate a crash from this version",
            )
        ]
    findings: List[Finding] = []
    if symbols.get("format") != SYMBOLS_FORMAT:
        findings.append(
            error(
                where,
                "manifests.artifact",
                "symbols.format is {!r}, expected {!r}".format(symbols.get("format"), SYMBOLS_FORMAT),
            )
        )
    if SHA256_RE.match(str(symbols.get("sha256", ""))) is None:
        findings.append(error(where, "manifests.artifact", "symbols.sha256 is not 64 hex"))
    size = symbols.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        findings.append(error(where, "manifests.artifact", "symbols.size must be a positive integer"))
    url = symbols.get("url")
    if url is not None:
        findings.extend(_check_url(where, "symbols.url", str(url), hosts))
    return findings


def _check_artifact_facts(
    where: str,
    manifest: Dict[str, Any],
    ledger: Dict[str, Any],
    channel_id: str,
    platform: str,
    version: SemVer,
) -> List[Finding]:
    release = None
    for candidate in ledger.get("releases", []):
        if candidate.get("channel") == channel_id and candidate.get("version") == str(version):
            release = candidate
            break
    if release is None:
        return [
            error(
                where,
                "manifests.artifact",
                "no ledger release records {} on {!r}".format(version, channel_id),
            )
        ]
    artifact = find_artifact(release, platform)
    if artifact is None:
        return [
            error(
                where,
                "manifests.artifact",
                "the ledger release has no {} artifact".format(platform),
            )
        ]
    findings: List[Finding] = []
    for field in ("sha256", "size"):
        if manifest.get(field) != artifact.get(field):
            findings.append(
                error(
                    where,
                    "manifests.artifact",
                    "{} does not match the ledger ({} vs {})".format(
                        field, manifest.get(field), artifact.get(field)
                    ),
                )
            )
    expected_symbols = artifact.get("symbols") or {}
    symbols = manifest.get("symbols") or {}
    if isinstance(symbols, dict):
        for field in ("artifact", "sha256", "size"):
            if symbols.get(field) != expected_symbols.get(field):
                findings.append(
                    error(
                        where,
                        "manifests.artifact",
                        "symbols.{} does not match the ledger ({} vs {})".format(
                            field, symbols.get(field), expected_symbols.get(field)
                        ),
                    )
                )
    if manifest.get("deprecations") != projected_deprecations(release):
        findings.append(
            error(where, "manifests.artifact", "deprecations do not match the ledger projection")
        )
    return findings


def _check_url(where: str, field: str, value: str, hosts: Sequence[str]) -> List[Finding]:
    if not value.startswith("https://"):
        return [error(where, "manifests.url", "{} {!r} is not https".format(field, value))]
    remainder = value[len("https://") :]
    host = remainder.split("/", 1)[0]
    if "@" in host or ":" in host:
        return [
            error(
                where,
                "manifests.url",
                "{} {!r} carries credentials or a port".format(field, value),
            )
        ]
    if host.lower() not in hosts:
        return [
            error(
                where,
                "manifests.url",
                "{} host {!r} is not in the channels.json allowlist ({})".format(
                    field, host, ", ".join(hosts) or "empty"
                ),
            )
        ]
    return []


def url_problem(value: str, hosts: Sequence[str]) -> Tuple[str, str]:
    """``(rule, message)`` for a bad URL, or ``("", "")``. Imported by the generator's self-check."""
    findings = _check_url("manifest", "url", value, hosts)
    return (findings[0].rule, findings[0].message) if findings else ("", "")
