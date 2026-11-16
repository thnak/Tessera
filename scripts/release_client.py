#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The update-client *policy* seam (issue #193, specification sections 3 and 6.1).

The specification's flow is ``check -> download -> verify -> stage -> apply -> health check``. Only the
decision half lands here, deliberately:

* :func:`plan_update` decides whether an update may be **offered** and how it may be **applied**,
  from the channel file and the ledger alone. It is pure: no transport, no filesystem, no clock.
* :class:`UpdateClientPolicy` is the interface an ``UpdateClient`` implementation implements. The
  state machine of section 6.1 (``Idle -> Checking -> ... -> Healthy/Rollback``) is represented as
  data, so a test can drive every transition without a network and a real client can be handed the
  same table.

Everything that needs the world stays out: downloading, resuming, hashing a partial file, staging a
directory, swapping slots, the health check and rollback. Each of those needs a transport or a
filesystem, and this repository's tests may use neither, so they are documented as the client
contract in ``docs/specs/updates-and-distribution-v1.md` section 6.3 and `docs/guides/packaging.md` section 15` rather than half-implemented here.

**Signing.** :func:`plan_update` takes ``signature_verified`` as an input. It cannot compute it --
verification needs the pinned public key and a signature produced by a private key this repository
does not hold. What the seam does guarantee is the refusal: an unsigned or unverified package is
never "offerable", so a caller cannot accidentally apply one by forgetting to check.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

from release_manifest import (
    channel_by_id,
    find_artifact,
    newest_release,
    transition,
)
from release_semver import SemVer

# The state machine of specification section 6.1, as data. `terminal` states need no further work;
# `rollback` is where a failed health check lands, and it is a terminal state on purpose: the client
# does not retry itself, it reports.
STATES: Tuple[str, ...] = (
    "Idle",
    "Checking",
    "UpToDate",
    "Available",
    "Downloading",
    "Verifying",
    "Staged",
    "Applying",
    "HealthCheck",
    "Healthy",
    "Rollback",
)
TERMINAL_STATES = ("UpToDate", "Healthy", "Rollback")

# Allowed transitions. `Offline` is not a state: section 2 says offline is a normal condition, so a
# failed check returns to `Idle` with a reason instead of parking the client.
TRANSITIONS: Dict[str, Tuple[str, ...]] = {
    "Idle": ("Checking",),
    "Checking": ("UpToDate", "Available", "Idle"),
    "Available": ("Downloading", "Idle"),
    "Downloading": ("Verifying", "Idle"),
    "Verifying": ("Staged", "Idle"),
    "Staged": ("Applying", "Idle"),
    "Applying": ("HealthCheck",),
    "HealthCheck": ("Healthy", "Rollback"),
    "UpToDate": ("Checking",),
    "Healthy": ("Checking",),
    "Rollback": ("Checking",),
}

# The number of failed launches that triggers a rollback (specification section 6.1, gap G-23).
ROLLBACK_FAILED_LAUNCHES = 2


def can_transition(source: str, target: str) -> bool:
    return target in TRANSITIONS.get(source, ())


def next_states(source: str) -> Tuple[str, ...]:
    return TRANSITIONS.get(source, ())


@dataclass(frozen=True)
class UpdateDecision:
    """What a client may do, decided offline from the channel file and the ledger.

    ``kind`` is one of ``up-to-date``, ``offer`` or ``blocked``; ``reason`` is the rule id that
    produced a refusal (``"none"`` otherwise), so the caller can log a stable identifier instead of
    parsing a sentence.
    """

    kind: str
    reason: str
    message: str
    version: str = ""
    apply_mode: str = ""  # the channel's consent mode; "" when there is nothing to apply
    signature: Optional[str] = None
    size: int = 0
    sha256: str = ""
    symbols_artifact: str = ""
    downgrade: bool = False

    @property
    def offered(self) -> bool:
        return self.kind == "offer"

    @property
    def blocked(self) -> bool:
        return self.kind == "blocked"


def plan_update(
    channels: Dict[str, Any],
    ledger: Dict[str, Any],
    *,
    installed_channel: str,
    installed_version: str,
    platform: str,
    target_channel: Optional[str] = None,
    consent: bool = False,
    signature_verified: bool = False,
    allow_downgrade: bool = False,
) -> UpdateDecision:
    """Decide whether ``installed_*`` may move to the newest release of ``target_channel``.

    Inputs, all explicit so a test needs no clock and no network:

    * ``consent`` -- the user asked for this channel move. A transition whose policy is ``optIn``
      (moving to a less stable channel) is refused without it.
    * ``signature_verified`` -- the caller's pinned-key store verified the manifest signature. This
      function cannot verify one; it refuses to apply when nothing did, so forgetting the check is a
      refusal rather than a silent install.
    * ``allow_downgrade`` -- the caller obtained an explicitly signed downgrade (section 6.1:
      "Downgrades are blocked unless explicitly signed"). Only meaningful when the channel file's
      downgrade policy is ``signed``.

    Refusals, in the order the specification applies them: unknown channel, channel move not
    permitted, no release for this platform, downgrade, unsigned package.
    """
    target = target_channel or installed_channel
    if channel_by_id(channels, target) is None:
        return UpdateDecision("blocked", "channels.id", "channel {!r} is not defined".format(target))
    if channel_by_id(channels, installed_channel) is None:
        return UpdateDecision(
            "blocked",
            "channels.id",
            "installed channel {!r} is not defined".format(installed_channel),
        )
    if target != installed_channel:
        row = transition(channels, installed_channel, target)
        if row is None:
            return UpdateDecision(
                "blocked",
                "channels.transition",
                "no transition from {!r} to {!r}".format(installed_channel, target),
            )
        if row.get("policy") == "optIn" and not consent:
            return UpdateDecision(
                "blocked",
                "channels.transition-direction",
                "moving to {!r} needs the user's explicit consent".format(target),
            )
    release = newest_release(ledger, target)
    if release is None:
        return UpdateDecision("up-to-date", "none", "no release is recorded on {!r}".format(target))
    artifact = find_artifact(release, platform)
    if artifact is None:
        return UpdateDecision(
            "up-to-date",
            "none",
            "the newest release on {!r} has no {} artifact".format(target, platform),
        )
    candidate = SemVer.parse(release.get("version"))
    installed = SemVer.parse(installed_version)
    if candidate is None:
        return UpdateDecision("blocked", "ledger.release", "the offered version is not SemVer")
    if installed is None:
        return UpdateDecision(
            "blocked", "ledger.release", "installed version {!r} is not SemVer".format(installed_version)
        )
    order = candidate.compare(installed)
    if order == 0:
        return UpdateDecision(
            "up-to-date", "none", "{} is installed".format(candidate), version=str(candidate)
        )
    if order < 0:
        policy = ""
        if isinstance(channels.get("downgrade"), dict):
            policy = str(channels["downgrade"].get("policy", ""))
        if policy != "signed" or not (allow_downgrade and signature_verified):
            return UpdateDecision(
                "blocked",
                "channels.downgrade",
                "{} is older than the installed {} and a downgrade needs an explicitly signed "
                "policy".format(candidate, installed),
                version=str(candidate),
                downgrade=True,
            )
    if release.get("signature") is None or not signature_verified:
        return UpdateDecision(
            "blocked",
            "manifests.shape",
            "{} is unsigned; a package is never applied without a verified signature".format(
                candidate
            ),
            version=str(candidate),
            downgrade=order < 0,
        )
    channel = channel_by_id(channels, target) or {}
    symbols = artifact.get("symbols") or {}
    return UpdateDecision(
        "offer",
        "none",
        "{} is available on {!r}".format(candidate, target),
        version=str(candidate),
        apply_mode=str(channel.get("consent", "prompt")),
        signature=str(release.get("signature")),
        size=int(artifact.get("size", 0)),
        sha256=str(artifact.get("sha256", "")),
        symbols_artifact=str(symbols.get("artifact", "")),
        downgrade=order < 0,
    )


def rollback_after(failed_launches: int) -> bool:
    """Whether the health check has failed enough times to roll back (G-23 sets N = 2)."""
    return failed_launches >= ROLLBACK_FAILED_LAUNCHES


def apply_allowed(decision: UpdateDecision, unsaved_work_active: bool) -> Tuple[bool, str]:
    """Whether the staged package may be applied now.

    Section 3: "No update while critical unsaved work is active; prompt/save first." The caller owns
    the unsaved-work signal; this decides on it so the rule has one implementation.
    """
    if not decision.offered:
        return False, "nothing is staged ({})".format(decision.kind)
    if unsaved_work_active:
        return False, "save or dismiss unsaved work before applying"
    return True, decision.apply_mode


class UpdateClientPolicy:
    """The interface an ``UpdateClient`` implementation satisfies.

    A concrete client needs a transport and a storage slot, neither of which may appear in a test in
    this repository. Landing the interface is the seam: a future implementation must answer these
    four questions, and :func:`plan_update` already answers the first two.
    """

    def plan(
        self,
        channels: Dict[str, Any],
        ledger: Dict[str, Any],
        *,
        installed_channel: str,
        installed_version: str,
        platform: str,
        target_channel: Optional[str] = None,
        consent: bool = False,
        signature_verified: bool = False,
        allow_downgrade: bool = False,
    ) -> UpdateDecision:
        return plan_update(
            channels,
            ledger,
            installed_channel=installed_channel,
            installed_version=installed_version,
            platform=platform,
            target_channel=target_channel,
            consent=consent,
            signature_verified=signature_verified,
            allow_downgrade=allow_downgrade,
        )

    def check(self) -> Any:  # pragma: no cover - the seam, not an implementation
        """Fetch the manifest. Needs a transport: not implemented in this repository yet."""
        raise NotImplementedError("an update client needs a transport; see docs/specs/updates-and-distribution-v1.md")

    def download(self, decision: UpdateDecision) -> Any:  # pragma: no cover - the seam
        raise NotImplementedError("download needs a transport; see docs/specs/updates-and-distribution-v1.md")

    def stage(self, package: Any) -> Any:  # pragma: no cover - the seam
        raise NotImplementedError("staging needs a filesystem slot; see docs/specs/updates-and-distribution-v1.md")

    def apply(self, decision: UpdateDecision) -> bool:  # pragma: no cover - the seam
        raise NotImplementedError("apply needs an installer; see docs/specs/updates-and-distribution-v1.md")


def state_path() -> List[str]:
    """The section 6.1 happy path, for a doc test to walk."""
    return ["Idle", "Checking", "Available", "Downloading", "Verifying", "Staged", "Applying",
            "HealthCheck", "Healthy"]
