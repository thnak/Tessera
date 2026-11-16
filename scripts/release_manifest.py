#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Release manifest model (issue #193, ``docs/specs/updates-and-distribution-v1.md``).

Data and deterministic derivation only: no policy, no filesystem walk, no clock, no network. The
policy lives in :mod:`release_policy`; the commands are ``gen_release_manifest.py`` and
``check_release_manifest.py``.

Three documents, one role each:

* ``release/channels.json`` -- the **channel file**: which channels exist, their stability rank, the
  consent mode each defaults to, the transition table for a device moving between them, the update
  host allowlist and the pinned signing keys. Hand-authored, reviewed like a baseline.
* ``release/ledger.json`` -- the **release ledger**: append-only history, one record per published
  version and channel. It is the only place a digest is recorded, and it is what lets the tooling
  answer "is this a downgrade?" without a clock.
* ``release/manifests/<channel>-<platform>.json`` -- the **wire manifest** a client reads
  (specification section 2). Derived from the ledger, never hand-edited, committed so drift is a
  gate failure instead of a surprise at publish time.

Determinism is a contract: every document is rendered with ``indent=2``, ``sort_keys=True``, ASCII
escapes and a single trailing newline, and nothing here reads the clock, the environment, a path or
a hostname. Two runs over the same inputs are byte-identical, which is what makes ``--check`` a
drift check rather than a coin flip.
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from release_semver import (
    DEPRECATION_DAY_WINDOW,
    DEPRECATION_MINOR_WINDOW,
    SemVer,
    deprecation_window,
    valid_date,
)

# Version of each document this module reads and writes (versioning-v1 section 2).
MANIFEST_VERSION = 1
LEDGER_VERSION = 1
CHANNELS_VERSION = 1

# Wire-format constants. The symbol artifact format is the one `crash_symbols.hpp` defines; the
# signature algorithm is the one the spec's gap closure (G-23/Q-22) pins.
SYMBOLS_FORMAT = "tessera.symbols.v1"
SIGNATURE_ALGORITHM = "ed25519"
CONSENT_MODES = ("onRestart", "prompt", "manual")
TRANSITION_POLICIES = ("free", "optIn", "blocked")
DOWNGRADE_POLICIES = ("blocked", "signed")
PLATFORMS = ("linux-x86_64", "macos-arm64", "windows-x86_64")

# The manifest keys specification section 2 lists, plus the ones this repository's artifacts need.
MANIFEST_FIELDS = (
    "manifestVersion",
    "channel",
    "version",
    "platform",
    "url",
    "sha256",
    "size",
    "signature",
    "minOs",
    "notesUrl",
    "symbols",
    "deprecations",
)

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
CHANNEL_ID_RE = re.compile(r"^[a-z][a-z0-9-]*$")
HOST_RE = re.compile(r"^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,}$")

_PLATFORM_ALTERNATION = "|".join(re.escape(platform) for platform in PLATFORMS)
ARCHIVE_NAME_RE = re.compile(
    r"^tessera-(?P<version>[0-9A-Za-z.+-]+)-(?P<platform>{})"
    r"(?P<suffix>\.tar\.gz|\.zip)$".format(_PLATFORM_ALTERNATION)
)
SYMBOLS_NAME_RE = re.compile(
    r"^tessera-(?P<version>[0-9A-Za-z.+-]+)-(?P<platform>{})\.symbols\.txt$".format(
        _PLATFORM_ALTERNATION
    )
)


class ManifestError(Exception):
    """Unreadable input, or a document that cannot be interpreted at all (exit code 2).

    A *policy* failure is deliberately not this: it is a :class:`release_policy.Finding`, because the
    gate must name every one of them instead of stopping at the first.
    """


# --------------------------------------------------------------------------------------------------
# Canonical IO
# --------------------------------------------------------------------------------------------------


def render(document: Any) -> str:
    """The one canonical serialization: sorted keys, two-space indent, LF, trailing newline."""
    return json.dumps(document, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def load_json(path: Path) -> Any:
    if not path.is_file():
        raise ManifestError("file not found: {}".format(path))
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ManifestError("{}: not valid JSON: {}".format(path, error))


def load_object(path: Path, label: str) -> Dict[str, Any]:
    document = load_json(path)
    if not isinstance(document, dict):
        raise ManifestError("{}: {} must be a JSON object".format(path, label))
    return document


def write_document(path: Path, document: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(render(document), encoding="utf-8", newline="")


def describe_drift(expected: str, actual: str) -> str:
    """The first differing line, as ``gen_sbom.py`` reports it: "stale" without the diff is work."""
    expected_lines = expected.splitlines()
    actual_lines = actual.splitlines()
    for index in range(max(len(expected_lines), len(actual_lines))):
        expected_line = expected_lines[index] if index < len(expected_lines) else "<end of file>"
        actual_line = actual_lines[index] if index < len(actual_lines) else "<end of file>"
        if expected_line != actual_line:
            return "first difference at line {}:\n  expected: {}\n  found:    {}".format(
                index + 1, expected_line, actual_line
            )
    return "content differs"


# --------------------------------------------------------------------------------------------------
# Artifacts on disk
# --------------------------------------------------------------------------------------------------


def archive_identity(path: Path) -> Tuple[str, str, str]:
    """``(version, platform, suffix)`` from the archive file name.

    The name is the contract ``check_package.py`` already enforces (exactly one top-level
    ``tessera-<version>-<os>-<arch>/`` directory), so a manifest cannot describe a differently named
    artifact without the two gates disagreeing.
    """
    match = ARCHIVE_NAME_RE.match(path.name)
    if match is None:
        raise ManifestError(
            "archive name {!r} is not tessera-<version>-<platform>.{{tar.gz,zip}} for one of "
            "{}".format(path.name, ", ".join(PLATFORMS))
        )
    return match.group("version"), match.group("platform"), match.group("suffix")


def symbols_identity(path: Path) -> Tuple[str, str]:
    """``(version, platform)`` from a ``tessera-<version>-<platform>.symbols.txt`` name."""
    match = SYMBOLS_NAME_RE.match(path.name)
    if match is None:
        raise ManifestError(
            "symbol artifact name {!r} is not tessera-<version>-<platform>.symbols.txt for one "
            "of {}".format(path.name, ", ".join(PLATFORMS))
        )
    return match.group("version"), match.group("platform")


def artifact_digest(path: Path) -> Tuple[int, str]:
    """``(size_bytes, sha256)`` of a file, streamed so a large archive is not held in memory."""
    if not path.is_file():
        raise ManifestError("artifact not found: {}".format(path))
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest()


def expected_archive_name(version: str, platform: str, suffix: str) -> str:
    return "tessera-{}-{}{}".format(version, platform, suffix)


def expected_symbols_name(version: str, platform: str) -> str:
    return "tessera-{}-{}.symbols.txt".format(version, platform)


def platform_suffix(platform: str) -> str:
    """The archive a platform's release uses: ZIP on Windows, TGZ elsewhere (packaging section 5)."""
    return ".zip" if platform.startswith("windows") else ".tar.gz"


def parse_symbol_header(text: str) -> Dict[str, Any]:
    """Structural read of a ``tessera.symbols.v1`` artifact (``crash_symbols.hpp``).

    This is not the resolver. It answers the distribution question a manifest must answer before a
    release ships: *is the symbol artifact we are publishing the one for this build, and does it
    carry names at all?* The resolver refuses a mismatched build id outright
    (crash-reporting-v1 section 6.3), so publishing such a file is worse than publishing none.
    """
    lines = text.splitlines()
    if not lines or lines[0].strip() != SYMBOLS_FORMAT:
        raise ManifestError("symbol artifact does not start with {!r}".format(SYMBOLS_FORMAT))
    build_id: Optional[str] = None
    modules = 0
    functions = 0
    for index, line in enumerate(lines[1:], start=2):
        if not line.strip():
            continue
        key, separator, value = line.partition("=")
        if not separator:
            raise ManifestError("symbol artifact line {}: expected key=value".format(index))
        if key == "build":
            if build_id is not None:
                raise ManifestError("symbol artifact line {}: duplicate build id".format(index))
            if COMMIT_RE.match(value.strip()) is None:
                raise ManifestError(
                    "symbol artifact line {}: build id is not a 40-hex commit".format(index)
                )
            build_id = value.strip()
        elif key == "module":
            if value.count("|") != 2:
                raise ManifestError("symbol artifact line {}: module row malformed".format(index))
            modules += 1
        elif key == "func":
            if value.count("|") < 3:
                raise ManifestError("symbol artifact line {}: func row malformed".format(index))
            functions += 1
    if build_id is None:
        raise ManifestError("symbol artifact has no build= line")
    if functions == 0:
        raise ManifestError("symbol artifact has no func= rows (it names nothing)")
    return {"build": build_id, "modules": modules, "functions": functions}


def read_package_info(path: Path) -> Dict[str, str]:
    """``PACKAGE-INFO.txt`` from inside a release archive, keyed by field.

    Reuses ``check_package.py``'s own archive reader and parser rather than opening a second,
    subtly different one: the two gates must agree about what the archive says.
    """
    if str(Path(__file__).resolve().parent) not in sys.path:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
    import check_package  # noqa: E402  (sibling script, stdlib only)

    try:
        archive = check_package.read_archive(path)
    except check_package.PackageError as error:
        raise ManifestError(str(error))
    members = [name for name in archive.files if name.rsplit("/", 1)[-1] == "PACKAGE-INFO.txt"]
    if len(members) != 1:
        raise ManifestError(
            "archive must contain exactly one PACKAGE-INFO.txt, found {}".format(len(members))
        )
    text = archive.files[members[0]].decode("utf-8", errors="replace")
    info, errors = check_package.parse_info(text)
    if errors:
        raise ManifestError("PACKAGE-INFO.txt: {}".format("; ".join(sorted(errors))))
    return info


# --------------------------------------------------------------------------------------------------
# Queries over the ledger
# --------------------------------------------------------------------------------------------------


def release_key(release: Dict[str, Any]) -> Tuple[Any, ...]:
    version = SemVer.parse(str(release.get("version", "")))
    return (
        str(release.get("channel", "")),
        version.precedence_key() if version else (0, 0, 0, 0, ()),
        str(release.get("date", "")),
    )


def canonical_ledger(ledger: Dict[str, Any]) -> Dict[str, Any]:
    """The ledger with its release list in canonical order (every other field preserved).

    Sorting makes the order total, so two ledgers holding the same records render identically
    whatever order they were appended in.
    """
    document = dict(ledger)
    releases = document.get("releases")
    if isinstance(releases, list):
        document["releases"] = sorted(
            (release for release in releases if isinstance(release, dict)), key=release_key
        )
    return document


def find_release(ledger: Dict[str, Any], channel: str, version: str) -> Optional[Dict[str, Any]]:
    for release in ledger.get("releases", []):
        if release.get("channel") == channel and release.get("version") == version:
            return release
    return None


def find_artifact(release: Dict[str, Any], platform: str) -> Optional[Dict[str, Any]]:
    for artifact in release.get("artifacts", []):
        if artifact.get("platform") == platform:
            return artifact
    return None


def releases_on_channel(ledger: Dict[str, Any], channel: str) -> List[Dict[str, Any]]:
    return [r for r in ledger.get("releases", []) if r.get("channel") == channel]


def newest_release(ledger: Dict[str, Any], channel: str) -> Optional[Dict[str, Any]]:
    """The highest-version release recorded on ``channel``; no clock is consulted."""
    candidates = releases_on_channel(ledger, channel)
    if not candidates:
        return None
    return max(candidates, key=release_key)


def channel_by_id(channels: Dict[str, Any], channel_id: str) -> Optional[Dict[str, Any]]:
    for channel in channels.get("channels", []):
        if channel.get("id") == channel_id:
            return channel
    return None


def transition(channels: Dict[str, Any], source: str, target: str) -> Optional[Dict[str, Any]]:
    for row in channels.get("transitions", []):
        if row.get("from") == source and row.get("to") == target:
            return row
    return None


def allowed_hosts(channels: Dict[str, Any]) -> List[str]:
    return [str(host).lower() for host in (channels.get("updateHosts") or [])]


def signing_keys(channels: Dict[str, Any]) -> List[Dict[str, Any]]:
    signing = channels.get("signing") or {}
    return [key for key in (signing.get("keys") or []) if isinstance(key, dict)]


# --------------------------------------------------------------------------------------------------
# Projection: ledger -> wire manifests
# --------------------------------------------------------------------------------------------------


def projected_deprecations(release: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The release's deprecations with both legal floors filled in.

    The computed ``earliestRemoval`` is what makes the two-minor-and-ninety-day window auditable in
    the artifact a client reads, instead of living only in a policy document.
    """
    projected: List[Dict[str, Any]] = []
    for entry in release.get("deprecations") or []:
        if not isinstance(entry, dict):
            continue
        deprecated = SemVer.parse(str(entry.get("deprecatedIn", "")))
        deprecated_date = str(entry.get("deprecatedDate", ""))
        if deprecated is None or not valid_date(deprecated_date):
            continue  # release_policy reports the malformed row; projection stays total
        version_floor, date_floor = deprecation_window(deprecated, deprecated_date)
        projected.append(
            {
                "entity": str(entry.get("entity", "")),
                "deprecatedIn": entry.get("deprecatedIn"),
                "removedIn": entry.get("removedIn"),
                "earliestRemoval": {"version": version_floor, "date": date_floor},
            }
        )
    return sorted(projected, key=lambda item: (item["entity"], str(item["deprecatedIn"])))


def build_manifest(
    release: Dict[str, Any], artifact: Dict[str, Any], channel: Dict[str, Any]
) -> Dict[str, Any]:
    """The wire manifest for one release artifact (specification section 2).

    ``url``/``notesUrl`` come from the ledger and stay ``null`` until a host is configured in
    ``release/channels.json``; ``check_release_manifest.py`` refuses a URL whose host is not on that
    allowlist, so a manifest can never point at a host a reviewer did not approve.
    """
    symbols = artifact.get("symbols")
    document: Dict[str, Any] = {
        "manifestVersion": MANIFEST_VERSION,
        "channel": str(channel["id"]),
        "version": str(release["version"]),
        "platform": str(artifact["platform"]),
        "url": artifact.get("url"),
        "sha256": str(artifact["sha256"]),
        "size": int(artifact["size"]),
        "signature": release.get("signature"),
        "minOs": release.get("minOs"),
        "notesUrl": artifact.get("notesUrl"),
        "symbols": None,
        "deprecations": projected_deprecations(release),
    }
    if isinstance(symbols, dict):
        document["symbols"] = {
            "artifact": str(symbols.get("artifact", "")),
            "sha256": str(symbols.get("sha256", "")),
            "size": int(symbols.get("size", 0)),
            "format": SYMBOLS_FORMAT,
            "url": symbols.get("url"),
        }
    return document


def project_manifests(
    ledger: Dict[str, Any], channels: Dict[str, Any]
) -> Dict[str, Dict[str, Any]]:
    """``{"<channel>-<platform>.json": document}`` for every ledger artifact, sorted by file name.

    One manifest per (channel, platform), carrying that channel's newest release for the platform: a
    client asks about its own channel and platform and gets exactly one answer. A platform the
    newest release does not ship is simply absent, so "no offer" is the absence of a manifest
    rather than a manifest with a hole in it.
    """
    projections: Dict[str, Dict[str, Any]] = {}
    channel_list = channels.get("channels")
    if not isinstance(channel_list, list):
        return projections
    for channel in channel_list:
        if not isinstance(channel, dict):
            continue
        channel_id = str(channel.get("id", ""))
        for platform in PLATFORMS:
            best: Optional[Tuple[Tuple[Any, ...], Dict[str, Any], Dict[str, Any]]] = None
            for release in releases_on_channel(ledger, channel_id):
                artifact = find_artifact(release, platform)
                if artifact is None:
                    continue
                version = SemVer.parse(str(release.get("version", "")))
                if version is None:
                    continue
                candidate = (version.precedence_key(), release, artifact)
                if best is None or candidate[0] > best[0]:
                    best = candidate
            if best is None:
                continue
            _, release, artifact = best
            projections[manifest_file_name(channel_id, platform)] = build_manifest(
                release, artifact, channel
            )
    return dict(sorted(projections.items()))


def manifest_file_name(channel: str, platform: str) -> str:
    return "{}-{}.json".format(channel, platform)


def manifest_path(manifests_dir: Path, channel: str, platform: str) -> Path:
    return manifests_dir / manifest_file_name(channel, platform)


def manifest_platforms(manifest: Dict[str, Any]) -> List[str]:
    platforms = []
    if manifest.get("platform"):
        platforms.append(str(manifest["platform"]))
    return platforms


def window_constants() -> Dict[str, int]:
    """The deprecation window, exposed so a caller can print and assert the same numbers."""
    return {"minor": DEPRECATION_MINOR_WINDOW, "days": DEPRECATION_DAY_WINDOW}
