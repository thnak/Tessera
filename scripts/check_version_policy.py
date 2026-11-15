#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Versioning-policy gate (issue #194): a breaking change to a gated surface needs a version bump.

Acceptance: *a documented policy and a CI check that fails on an unversioned breaking change to the
RHI headers, IR schema or ``tessera.json``*. The policy is
[`docs/specs/versioning-v1.md`](../docs/specs/versioning-v1.md) section 11; this is the check. It is
part of one surface per module:

* :mod:`version_surface` reads the three surfaces and their versions out of the tree,
* :mod:`version_baseline` records the reviewed state in ``scripts/versioning-baseline.txt``,
* :mod:`version_policy_rules` decides what a difference means (the policy as code),
* this file is the command: parse arguments, run the rules, print, exit.

What the gate fails, and why:

* **a field was removed, retyped, made required, or had its default changed, and the version did not
  move** -> every such field is named. This is the unversioned breaking change the issue asks the
  gate to refuse.
* **any other difference** (a new optional field, or a version move) -> the author must record it
  with ``--update``. The record is an exact snapshot, not a floor: every difference is recorded in
  the same change, which is what stops a retype of a field added since the last recording from
  slipping past as "new since the baseline", and what keeps a bump and its baseline from being split
  across two commits.
* ``--update`` itself refuses a breaking change whose version did not move, so it can only record a
  decision, never hide one.

**Not a second public-API listing gate.** Issue #76 (``scripts/check_public_api.py``) records *that*
the public surface changed, by listing the symbols of ``include/tessera/**`` and diffing the list.
This gate records *whether that change required a version bump*. It never reads #76's script or
baseline and it keeps its own narrow input set, so the two land and rebase independently. RHI is a
digest here, not a symbol list, on purpose: classifying an RHI edit as additive or breaking from the
headers is #76's job, and a second classifier would be exactly the overlapping mechanism this change
avoids. The rule is the one ADR-049 already states --- any change to the contract moves a version,
and the author states which kind.

It also does not duplicate the file-touch rule in ``scripts/check_docs.py`` (an RHI header change must
touch ``version.hpp`` and ``docs/design/gpu/rhi.md``): that rule answers "which files were edited",
this one answers "did the declared version actually move".

Output is deterministic and sorted: ``ERROR path: message`` / ``WARN path: message``, byte-identical
across runs; no clock, network, locale or random value is consulted. Exit codes: 0 = no errors
(warnings are fine), 1 = at least one error, 2 = a surface input could not be read.

Usage:
  python -B scripts/check_version_policy.py --check
  python -B scripts/check_version_policy.py --update    # re-record after a reviewed change
  python -B scripts/check_version_policy.py --verbose   # also list every gated surface
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))
import version_baseline as baseline  # noqa: E402  (sibling script, stdlib only)
import version_policy_rules as rules  # noqa: E402  (sibling script, stdlib only)
import version_surface as surface  # noqa: E402  (sibling script, stdlib only)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="compare the tree against the recorded surfaces (the default)",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="re-record the surfaces after a reviewed change (never edits the version itself)",
    )
    parser.add_argument(
        "--verbose", action="store_true", help="also list every gated surface and its version"
    )
    parser.add_argument(
        "--repo-root",
        default=str(REPO_ROOT),
        help="repository root (default: the checkout containing this script)",
    )
    args = parser.parse_args(argv)
    root = Path(args.repo_root).resolve()

    try:
        findings, warnings, current = rules.evaluate(root)
    except surface.SurfaceError as error:
        print("ERROR {}: {}".format(baseline.BASELINE, error), file=sys.stderr)
        return 2

    if args.verbose:
        for name in rules.SURFACE_ORDER:
            counted = len(current[name].get("fields") or {}) or 1
            print(
                "note: {} version {} ({} input(s))".format(
                    rules.SURFACE_LABEL[name], rules.describe_version(current[name]["version"]), counted
                )
            )

    # An update is only a recording step: it refuses while a version did not move for a breaking
    # change, so `--update` can never launder the failure this gate exists to report.
    if args.update:
        blocking = [finding for finding in findings if finding.kind in rules.UNRECORDABLE]
        if blocking:
            for finding in sorted(blocking, key=lambda item: item.message):
                print("ERROR " + finding.message)
            print(
                "version-policy gate: refusing --update with {} unresolved error(s); bump the "
                "version first".format(len(blocking)),
                file=sys.stderr,
            )
            return 1
        baseline.baseline_path(root).write_text(
            baseline.dump_baseline(current), encoding="utf-8", newline="\n"
        )
        print(
            "versioning baseline written: {} surface(s) at {}".format(
                len(current),
                ", ".join(
                    "{} {}".format(name, rules.describe_version(current[name]["version"]))
                    for name in rules.SURFACE_ORDER
                ),
            )
        )
        return 0

    for message in sorted(warnings):
        print("WARN " + message)
    for finding in findings:
        print("ERROR " + finding.message)
    if findings:
        print(
            "version-policy gate: {} error(s), {} warning(s), {} surface(s) checked".format(
                len(findings), len(warnings), len(current)
            ),
            file=sys.stderr,
        )
        return 1
    print(
        "version-policy gate: 0 error(s), {} warning(s), {} surface(s) checked".format(
            len(warnings), len(current)
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
