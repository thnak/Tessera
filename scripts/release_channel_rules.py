#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""``release/channels.json`` rules (issue #193, specification section 1 and 6.1).

The channel file is the data half of "who may move between channels"; this module is the half that
checks it. What it enforces, all of it a statement the specification already makes:

* **`stable` and `beta` exist; `nightly` is optional.** Section 1 names the three and marks
  `nightly` optional per app.
* **Ranks are distinct and `stable` is the most stable (rank 0).** A rank is what makes "more
  stable" a comparison instead of a hand-maintained matrix.
* **Every ordered pair of channels has exactly one transition row.** Silence is not a policy: a
  missing row would be either an accidental block or an accidental permission, and neither is
  reviewable.
* **Direction.** Moving to a *less* stable channel is always ``optIn`` (the user must ask); moving
  to a *more* stable one is ``free`` (a promotion). That is the whole "who may move" answer.
* **The downgrade policy is explicit** -- ``blocked`` (the V1 default) or ``signed`` -- because
  section 3 says a downgrade needs an explicit policy, never a silent one.

Nothing here verifies a signature: see :mod:`release_client` for what the seam refuses instead.
"""

from __future__ import annotations

from typing import Any, Dict, List, Tuple

from release_data_rules import check_hosts, check_signing
from release_findings import Finding, error
from release_manifest import (
    CHANNEL_ID_RE,
    CHANNELS_VERSION,
    CONSENT_MODES,
    DOWNGRADE_POLICIES,
)

# Channels the spec names (section 1). `stable` and `beta` are required; `nightly` is optional per
# app, so it is legal data and not required data.
REQUIRED_CHANNELS = ("stable", "beta")
OPTIONAL_CHANNELS = ("nightly",)

KNOWN_CHANNEL_IDS = REQUIRED_CHANNELS + OPTIONAL_CHANNELS


def check_channels(channels: Any, path: str = "release/channels.json") -> List[Finding]:
    findings: List[Finding] = []
    if not isinstance(channels, dict):
        return [error(path, "channels.version", "the channel file must be a JSON object")]
    if channels.get("channelsVersion") != CHANNELS_VERSION:
        findings.append(
            error(
                path,
                "channels.version",
                "channelsVersion must be {} (found {!r})".format(
                    CHANNELS_VERSION, channels.get("channelsVersion")
                ),
            )
        )

    declared = channels.get("channels")
    if not isinstance(declared, list) or not declared:
        return findings + [error(path, "channels.id", "channels must be a non-empty array")]

    seen: Dict[str, Dict[str, Any]] = {}
    ranks: Dict[int, str] = {}
    for index, channel in enumerate(declared):
        if not isinstance(channel, dict):
            findings.append(
                error(path, "channels.id", "channels[{}] is not an object".format(index))
            )
            continue
        channel_id = str(channel.get("id", ""))
        if CHANNEL_ID_RE.match(channel_id) is None:
            findings.append(
                error(
                    path,
                    "channels.id",
                    "channel id {!r} is not lower-case [a-z0-9-]".format(channel_id),
                )
            )
            continue
        if channel_id in seen:
            findings.append(
                error(path, "channels.id", "duplicate channel id {!r}".format(channel_id))
            )
        seen[channel_id] = channel
        if channel_id not in KNOWN_CHANNEL_IDS:
            findings.append(
                error(
                    path,
                    "channels.id",
                    "channel {!r} is not one of {}".format(
                        channel_id, ", ".join(KNOWN_CHANNEL_IDS)
                    ),
                )
            )
        findings.extend(_check_channel_shape(path, channel_id, channel, ranks))
    findings.extend(_check_required_channels(path, seen))
    findings.extend(_check_transitions(path, channels, seen))
    findings.extend(check_hosts(path, channels))
    findings.extend(check_signing(path, channels))
    return findings


def _check_channel_shape(
    path: str, channel_id: str, channel: Dict[str, Any], ranks: Dict[int, str]
) -> List[Finding]:
    findings: List[Finding] = []
    rank = channel.get("rank")
    if not isinstance(rank, int) or isinstance(rank, bool) or rank < 0:
        findings.append(
            error(
                path,
                "channels.rank",
                "channel {!r} needs a non-negative integer rank".format(channel_id),
            )
        )
    elif rank in ranks and ranks[rank] != channel_id:
        findings.append(
            error(
                path,
                "channels.rank",
                "channels {!r} and {!r} share rank {}".format(ranks[rank], channel_id, rank),
            )
        )
    else:
        ranks[rank] = channel_id
    consent = str(channel.get("consent", ""))
    if consent not in CONSENT_MODES:
        findings.append(
            error(
                path,
                "channels.consent",
                "channel {!r} consent is {!r}; expected one of {}".format(
                    channel_id, consent, ", ".join(CONSENT_MODES)
                ),
            )
        )
    if not isinstance(channel.get("prerelease"), bool):
        findings.append(
            error(
                path,
                "channels.id",
                "channel {!r} needs a boolean prerelease flag".format(channel_id),
            )
        )
    return findings


def _check_required_channels(path: str, seen: Dict[str, Dict[str, Any]]) -> List[Finding]:
    findings: List[Finding] = []
    for required in REQUIRED_CHANNELS:
        if required not in seen:
            findings.append(
                error(path, "channels.required", "the {!r} channel is not defined".format(required))
            )
    stable = seen.get("stable")
    if isinstance(stable, dict):
        if isinstance(stable.get("rank"), int) and stable["rank"] != 0:
            findings.append(
                error(path, "channels.rank", "the stable channel must have rank 0 (most stable)")
            )
        if stable.get("prerelease") is not False:
            findings.append(
                error(path, "channels.id", "the stable channel must not accept pre-releases")
            )
    for channel_id, channel in sorted(seen.items()):
        if channel_id == "stable" or not isinstance(channel, dict):
            continue
        if channel.get("prerelease") is not True:
            findings.append(
                error(
                    path,
                    "channels.id",
                    "the {!r} channel must set prerelease: true".format(channel_id),
                )
            )
    return findings


def _check_transitions(
    path: str, channels: Dict[str, Any], seen: Dict[str, Dict[str, Any]]
) -> List[Finding]:
    findings: List[Finding] = []
    rows = channels.get("transitions")
    if not isinstance(rows, list):
        return [error(path, "channels.transition", "transitions must be an array")]
    by_pair: Dict[Tuple[str, str], Dict[str, Any]] = {}
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            findings.append(
                error(path, "channels.transition", "transitions[{}] is not an object".format(index))
            )
            continue
        source, target = str(row.get("from", "")), str(row.get("to", ""))
        if source not in seen or target not in seen:
            findings.append(
                error(
                    path,
                    "channels.transition",
                    "transition {} -> {} names an undefined channel".format(
                        source or "?", target or "?"
                    ),
                )
            )
            continue
        if source == target:
            findings.append(
                error(
                    path,
                    "channels.transition",
                    "transition {} -> {} is a no-op".format(source, target),
                )
            )
            continue
        if (source, target) in by_pair:
            findings.append(
                error(
                    path,
                    "channels.transition",
                    "duplicate transition {} -> {}".format(source, target),
                )
            )
            continue
        policy = str(row.get("policy", ""))
        if policy not in ("free", "optIn"):
            findings.append(
                error(
                    path,
                    "channels.transition",
                    "transition {} -> {} policy {!r} is not free/optIn (a downgrade is the "
                    "channel file's downgrade rule, not a transition row)".format(
                        source, target, policy
                    ),
                )
            )
        by_pair[(source, target)] = row

    for source in sorted(seen):
        for target in sorted(seen):
            if source != target and (source, target) not in by_pair:
                findings.append(
                    error(
                        path,
                        "channels.transition",
                        "no transition row for {} -> {}; the policy must be explicit".format(
                            source, target
                        ),
                    )
                )

    for (source, target), row in sorted(by_pair.items()):
        findings.extend(_check_direction(path, seen, source, target, row))

    policy = channels.get("downgrade")
    if not isinstance(policy, dict):
        findings.append(error(path, "channels.downgrade", "downgrade must be an object"))
    elif str(policy.get("policy", "")) not in DOWNGRADE_POLICIES:
        findings.append(
            error(
                path,
                "channels.downgrade",
                "downgrade policy {!r} is not one of {}".format(
                    policy.get("policy"), ", ".join(DOWNGRADE_POLICIES)
                ),
            )
        )
    return findings


def _check_direction(
    path: str,
    seen: Dict[str, Dict[str, Any]],
    source: str,
    target: str,
    row: Dict[str, Any],
) -> List[Finding]:
    source_rank = seen[source].get("rank")
    target_rank = seen[target].get("rank")
    if not isinstance(source_rank, int) or not isinstance(target_rank, int):
        return []
    policy = str(row.get("policy", ""))
    if target_rank > source_rank and policy != "optIn":
        return [
            error(
                path,
                "channels.transition-direction",
                "{} -> {} moves to a less stable channel, so it must be optIn (found {!r})".format(
                    source, target, policy
                ),
            )
        ]
    if target_rank < source_rank and policy != "free":
        return [
            error(
                path,
                "channels.transition-stable",
                "{} -> {} moves to a more stable channel, so it must be free (found {!r})".format(
                    source, target, policy
                ),
            )
        ]
    return []
