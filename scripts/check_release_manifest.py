#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Distribution gate: release manifest, channels and the update policy (issue #193).

The command half of ``docs/specs/updates-and-distribution-v1.md``: it reads ``release/channels.json``,
``release/ledger.json`` and the committed ``release/manifests/``, applies the rules in the
``release_*_rules`` modules, and prints one sorted finding per line.

What it fails, and why:

* a channel file that does not define ``stable`` and ``beta``, ranks two channels the same, leaves a
  channel move without a transition row, or makes a move to a *less* stable channel ``free`` -- the
  "who may move between channels" policy;
* a ledger release whose archive name, digest, symbol artifact or build id does not describe the
  shipped bytes, whose version is already recorded on that channel, or that is newer than
  ``project(Tessera VERSION ...)``;
* a deprecation removed before **two minor releases and ninety days** have passed
  (versioning-v1 section 11.5);
* a manifest that is not byte-identical to the ledger projection, or that points at a host nobody
  allowlisted -- so a copied-in manifest cannot invent an update source.

It deliberately does **not** verify a signature, fetch a URL or read the clock. The signature needs a
private key this repository does not hold; the URL check is a string comparison against the channel
file; and every date comes from the data, so two runs on the same tree print identical bytes.

Usage (from the repository root; the runner is cwd-independent):

    python -B scripts/check_release_manifest.py
    python -B scripts/check_release_manifest.py --json

Exit codes: 0 no errors (warnings are fine), 1 at least one error, 2 unreadable input.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import release_manifest as model  # noqa: E402  (sibling module, stdlib only)
import release_manifests_io as io  # noqa: E402
from release_policy import evaluate  # noqa: E402

DEFAULT_CHANNELS = REPO_ROOT / "release" / "channels.json"
DEFAULT_LEDGER = REPO_ROOT / "release" / "ledger.json"
DEFAULT_MANIFESTS = REPO_ROOT / "release" / "manifests"
DEFAULT_CMAKE = REPO_ROOT / "CMakeLists.txt"


def _configure_streams() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(errors="backslashreplace")
            except (ValueError, OSError):
                pass


def collect(
    channels_path: Path,
    ledger_path: Path,
    manifests_dir: Path,
    cmake_path: Path,
) -> tuple:
    channels = model.load_object(channels_path, "the channel file")
    ledger = model.load_object(ledger_path, "the ledger")
    committed = io.read_committed(manifests_dir)
    findings = evaluate(
        channels,
        ledger,
        committed,
        io.project_version(cmake_path),
        io.rel(channels_path),
        io.rel(ledger_path),
        io.rel(manifests_dir),
    )
    return channels, ledger, committed, findings


def summarize(channels: Dict[str, Any], ledger: Dict[str, Any], findings: List[Any]) -> Dict[str, Any]:
    releases = [release for release in ledger.get("releases", []) if isinstance(release, dict)]
    channels_declared = [
        channel for channel in (channels.get("channels") or []) if isinstance(channel, dict)
    ]
    signing = channels.get("signing") or {}
    return {
        "ok": not any(finding.severity == "error" for finding in findings),
        "gate": "release-manifest",
        "channels": [str(channel.get("id")) for channel in channels_declared],
        "releases": len(releases),
        "artifacts": sum(len(release.get("artifacts") or []) for release in releases),
        "pinned_keys": len(signing.get("keys") or []),
        "errors": [finding.format() for finding in findings if finding.severity == "error"],
        "warnings": [finding.format() for finding in findings if finding.severity == "warn"],
    }


def main(argv: Optional[Sequence[str]] = None) -> int:
    _configure_streams()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--channels", default=str(DEFAULT_CHANNELS))
    parser.add_argument("--ledger", default=str(DEFAULT_LEDGER))
    parser.add_argument("--manifests", default=str(DEFAULT_MANIFESTS))
    parser.add_argument("--cmake-lists", default=str(DEFAULT_CMAKE))
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    try:
        channels, ledger, _committed, findings = collect(
            Path(args.channels),
            Path(args.ledger),
            Path(args.manifests),
            Path(args.cmake_lists),
        )
    except model.ManifestError as error:
        message = str(error)
        setup = "file not found" in message
        if args.as_json:
            print(
                json.dumps(
                    {"ok": False, "gate": "release-manifest", "errors": [message]},
                    indent=2,
                    sort_keys=True,
                )
            )
        print("error: {}".format(message), file=sys.stderr)
        return 2 if setup else 1

    errors = [finding for finding in findings if finding.severity == "error"]
    warnings = [finding for finding in findings if finding.severity == "warn"]
    payload = summarize(channels, ledger, findings)
    if args.as_json:
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        for finding in findings:
            print(finding.format())
        print(
            "release-manifest gate: {} error(s), {} warning(s), {} release(s), {} artifact(s)".format(
                len(errors), len(warnings), payload["releases"], payload["artifacts"]
            )
        )
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
