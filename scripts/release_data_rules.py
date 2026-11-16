#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-allowlist and pinned-key rules shared by the distribution documents (issue #193).

Two rules that both the channel file and a manifest must satisfy, kept in one place so the two can
never drift:

* **The allowlist is hosts, not URLs.** Specification section 5: "No update URL from untrusted
  input; allowlist hosts". A full URL in the data would hard-code a scheme, a path and possibly
  credentials that nobody reviewed, so only ``example.com`` is legal and the manifest composes the
  URL.
* **Keys are Ed25519, pinned by id, with a validity date.** The algorithm is the one the gap
  closure pins (G-23/Q-22). An empty key list is the *unsigned* state and is legal: it produces a
  warning, not an error, because the real key needs a certificate and a CI secret that this
  repository does not have yet (see ``docs/specs/updates-and-distribution-v1.md` section 6.3 and `docs/guides/packaging.md` section 15`).
"""

from __future__ import annotations

import re
from typing import Any, Dict, List

from release_findings import Finding, error, warn
from release_manifest import HOST_RE, SIGNATURE_ALGORITHM
from release_semver import valid_date

PUBLIC_KEY_RE = re.compile(r"^[0-9a-f]{64}$")  # Ed25519 public key: 32 bytes
KEY_STATUSES = ("active", "retired")


def check_hosts(path: str, document: Dict[str, Any]) -> List[Finding]:
    """``updateHosts`` is a list of bare lower-case DNS names; an empty list is legal."""
    hosts = document.get("updateHosts")
    if hosts is None:
        return []
    if not isinstance(hosts, list):
        return [error(path, "channels.hosts", "updateHosts must be an array (possibly empty)")]
    findings: List[Finding] = []
    seen = set()
    for host in hosts:
        text = str(host)
        if text != text.lower():
            findings.append(
                error(path, "channels.hosts", "host {!r} must be lower-case".format(text))
            )
        if "://" in text or "/" in text or ":" in text:
            findings.append(
                error(
                    path,
                    "channels.hosts",
                    "{!r} is a URL, not a bare host; the manifest builds the URL".format(text),
                )
            )
            continue
        if HOST_RE.match(text) is None:
            findings.append(
                error(path, "channels.hosts", "{!r} is not a DNS host name".format(text))
            )
        if text in seen:
            findings.append(error(path, "channels.hosts", "duplicate host {!r}".format(text)))
        seen.add(text)
    return findings


def check_signing(path: str, channels: Dict[str, Any]) -> List[Finding]:
    """The pinned Ed25519 keys, or the explicit unsigned state."""
    signing = channels.get("signing")
    if not isinstance(signing, dict):
        return [error(path, "channels.signing", "signing must be an object")]
    findings: List[Finding] = []
    algorithm = str(signing.get("algorithm", ""))
    if algorithm != SIGNATURE_ALGORITHM:
        findings.append(
            error(
                path,
                "channels.signing",
                "algorithm is {!r}; the spec pins {!r}".format(algorithm, SIGNATURE_ALGORITHM),
            )
        )
    keys = signing.get("keys")
    if not isinstance(keys, list):
        return findings + [error(path, "channels.signing", "signing.keys must be an array")]
    seen = set()
    for index, key in enumerate(keys):
        if not isinstance(key, dict):
            findings.append(
                error(path, "channels.signing", "signing.keys[{}] is not an object".format(index))
            )
            continue
        key_id = str(key.get("keyId", ""))
        label = key_id or "#{}".format(index)
        if not key_id:
            findings.append(
                error(path, "channels.signing", "signing.keys[{}] has no keyId".format(index))
            )
        elif key_id in seen:
            findings.append(error(path, "channels.signing", "duplicate keyId {!r}".format(key_id)))
        seen.add(key_id)
        if PUBLIC_KEY_RE.match(str(key.get("publicKey", ""))) is None:
            findings.append(
                error(
                    path,
                    "channels.signing",
                    "key {!r} publicKey must be 32 bytes of lower-case hex".format(label),
                )
            )
        if key.get("status") not in KEY_STATUSES:
            findings.append(
                error(
                    path,
                    "channels.signing",
                    "key {!r} status must be one of {}".format(label, ", ".join(KEY_STATUSES)),
                )
            )
        if not valid_date(key.get("notBefore")):
            findings.append(
                error(
                    path,
                    "channels.signing",
                    "key {!r} needs a notBefore date (YYYY-MM-DD)".format(label),
                )
            )
    if not keys:
        findings.append(
            warn(
                path,
                "channels.signing",
                "no signing key is pinned: releases are unsigned and the update policy refuses to "
                "apply one until the release owner provides a key",
            )
        )
    return findings


def signature_is_hex(value: Any, length: int = 128) -> bool:
    """A signature is 64 bytes of lower-case hex (Ed25519)."""
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{{{}}}".format(length), value) is not None
