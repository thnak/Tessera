#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""SemVer precedence and the deprecation window (issue #193).

The two pieces of arithmetic the release-manifest tooling needs, kept apart from the document model
because they are pure and are where the subtle rules live:

* **SemVer 2.0 section 11 precedence.** A pre-release sorts *before* the release it leads up to, and
  numeric pre-release identifiers compare as numbers, so ``1.0.0-beta.10`` is after
  ``1.0.0-beta.9``. Channel membership is decided by these comparisons, so getting them wrong would
  offer a downgrade.
* **The two-and-ninety rule** ([`versioning-v1.md`](../../docs/specs/versioning-v1.md) section 11.5,
  ADR-049 item 5). A deprecated entity stays for at least **two minor releases and ninety days,
  whichever is longer**, counted from the release that first deprecated it. The ninety days are
  arithmetic over a date the *data* supplies, never over the clock: a test or a gate that asked
  "has it been ninety days?" by reading today's date would fail on the day it was written wrong and
  would be non-deterministic by construction.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from datetime import date, timedelta
from typing import Any, Optional, Tuple

DEPRECATION_MINOR_WINDOW = 2
DEPRECATION_DAY_WINDOW = 90

SEMVER_RE = re.compile(
    r"^(?P<major>0|[1-9][0-9]*)\.(?P<minor>0|[1-9][0-9]*)\.(?P<patch>0|[1-9][0-9]*)"
    r"(?:-(?P<pre>[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+(?P<build>[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


@dataclass(frozen=True, order=True)
class SemVer:
    """A SemVer 2.0 version.

    ``order=True`` covers ``major``/``minor``/``patch``; :meth:`precedence_key` adds the pre-release
    ordering, and :meth:`compare` is the one comparison callers should use.
    """

    major: int
    minor: int
    patch: int
    prerelease: Tuple[str, ...] = ()
    build: Optional[str] = None

    @classmethod
    def parse(cls, text: Any) -> Optional["SemVer"]:
        match = SEMVER_RE.match(text) if isinstance(text, str) else None
        if match is None:
            return None
        prerelease = tuple(match.group("pre").split(".")) if match.group("pre") else ()
        return cls(
            int(match.group("major")),
            int(match.group("minor")),
            int(match.group("patch")),
            prerelease,
            match.group("build"),
        )

    def precedence_key(self) -> Tuple[int, int, int, int, Tuple[Tuple[int, Any], ...]]:
        identifiers = []
        for identifier in self.prerelease:
            # Numeric identifiers compare numerically and rank below alphanumeric ones.
            identifiers.append((0, int(identifier)) if identifier.isdigit() else (1, identifier))
        # A release outranks every pre-release of the same X.Y.Z, hence the 1/0 flag.
        return (self.major, self.minor, self.patch, 0 if self.prerelease else 1, tuple(identifiers))

    def __str__(self) -> str:
        text = "{}.{}.{}".format(self.major, self.minor, self.patch)
        if self.prerelease:
            text += "-" + ".".join(self.prerelease)
        if self.build:
            text += "+" + self.build
        return text

    def compare(self, other: "SemVer") -> int:
        left, right = self.precedence_key(), other.precedence_key()
        return (left > right) - (left < right)

    def bump_class(self, other: "SemVer") -> str:
        """``major``/``minor``/``patch`` for ``self -> other``, or ``none`` when not an increase."""
        if other.compare(self) <= 0:
            return "none"
        if other.major != self.major:
            return "major"
        if other.minor != self.minor:
            return "minor"
        if other.patch != self.patch:
            return "patch"
        return "none"  # same core version: only the pre-release moved


def minor_distance(deprecated: SemVer, later: SemVer) -> int:
    """Minor releases between the two, or -1 when ``later`` does not follow ``deprecated``.

    A major bump satisfies the window outright: every minor of the old line is behind it, which is
    why it reports the full window rather than a negative distance.
    """
    if later.compare(deprecated) <= 0:
        return -1
    if later.major != deprecated.major:
        return DEPRECATION_MINOR_WINDOW
    return later.minor - deprecated.minor


def earliest_removal_version(deprecated: SemVer) -> SemVer:
    return SemVer(deprecated.major, deprecated.minor + DEPRECATION_MINOR_WINDOW, 0)


def plus_days(day: str, days: int) -> str:
    """``YYYY-MM-DD`` plus ``days``, over a date the data supplies."""
    return (date.fromisoformat(day) + timedelta(days=days)).isoformat()


def valid_date(text: Any) -> bool:
    if not isinstance(text, str) or DATE_RE.match(text) is None:
        return False
    try:
        date.fromisoformat(text)
    except ValueError:
        return False
    return True


def deprecation_window(deprecated: SemVer, deprecated_date: str) -> Tuple[str, str]:
    """``(earliest_removal_version, earliest_removal_date)`` for a deprecation.

    Both floors are returned because the rule is "whichever is longer": a caller that compares only
    one of them can remove an entity early through the other.
    """
    return (
        str(earliest_removal_version(deprecated)),
        plus_days(deprecated_date, DEPRECATION_DAY_WINDOW),
    )
