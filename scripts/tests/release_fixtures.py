#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared fixtures for the release-manifest tests (issue #193).

Not a test module: no ``test_*`` name, so unittest discovery ignores it. It holds the constants and
the builders the two test modules share, so a change to the ledger shape is made in one place:

* a real commit id and the archive/symbol names the package layout produces for it,
* a valid two-channel file (``stable``/``beta``) with the direction rules satisfied,
* a release record whose digests are synthetic but whose *names* are the real ones,
* a minimal archive carrying ``PACKAGE-INFO.txt``, which is what ``record`` reads,
* a throwaway repository under ``tempfile`` with its own channels/ledger/manifests/CMakeLists.

The fixture archive is deliberately a ``.zip`` file name but a tar.gz payload: ``record`` reads the
archive through ``check_package.read_archive``, which sniffs the format rather than trusting the
suffix, so this proves the reader is format-agnostic while keeping the name the layout expects.
"""

from __future__ import annotations

import io
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, Optional

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
GENERATOR = SCRIPTS_DIR / "gen_release_manifest.py"
CHECKER = SCRIPTS_DIR / "check_release_manifest.py"

if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import release_manifest as model  # noqa: E402

COMMIT = "79fff887d6fe1d05af93ffeb80aed0942f388901"
VERSION = "0.0.1"
PLATFORM = "windows-x86_64"

CHANNELS: Dict[str, Any] = {
    "channelsVersion": 1,
    "channels": [
        {"id": "stable", "rank": 0, "prerelease": False, "consent": "prompt"},
        {"id": "beta", "rank": 1, "prerelease": True, "consent": "prompt"},
    ],
    "transitions": [
        {"from": "stable", "to": "beta", "policy": "optIn"},
        {"from": "beta", "to": "stable", "policy": "free"},
    ],
    "downgrade": {"policy": "blocked"},
    "updateHosts": [],
    "signing": {"algorithm": "ed25519", "keys": []},
}


def symbols_text(commit: str = COMMIT, functions: int = 3) -> str:
    """A `tessera.symbols.v1` file in the exact shape `write_symbol_file` emits.

    The C++ writer (`src/diagnostics/crash_symbols.cpp`) writes `func=<module index>|<start>|<size>|
    <name>`: the first field is the *index* of a `module=` row, not the module's name. A fixture
    that used the name would pass a parser that never checks the field, so it uses the index.
    """
    lines = ["tessera.symbols.v1", "build=" + commit, "module=app.exe|0x140000000|0x1000"]
    lines.extend(
        "func=0|{}|16|tessera::f{}".format(0x10 * (index + 1), index) for index in range(functions)
    )
    return "\n".join(lines) + "\n"


def package_info(version: str = VERSION, platform: str = PLATFORM, commit: str = COMMIT) -> str:
    return (
        "Tessera package\n"
        "Version: {}\n"
        "Commit: {}\n"
        "Platform: {}\n"
        "StaticRuntime: ON\n"
        "Executables: app\n".format(version, commit, platform)
    )


def write_archive(path: Path, info: str) -> Path:
    """A minimal archive carrying only PACKAGE-INFO.txt: enough for the ledger's own reader."""
    payload = info.encode("utf-8")
    with tarfile.open(path, "w:gz") as archive:
        member = tarfile.TarInfo("tessera-{}-{}/PACKAGE-INFO.txt".format(VERSION, PLATFORM))
        member.size = len(payload)
        member.mtime = 0
        archive.addfile(member, io.BytesIO(payload))
    return path


def release_record(
    version: str = VERSION,
    channel: str = "beta",
    day: str = "2026-11-16",
    signature: Optional[str] = None,
    deprecations: Optional[list] = None,
) -> Dict[str, Any]:
    return {
        "channel": channel,
        "version": version,
        "date": day,
        "commit": COMMIT,
        "signature": signature,
        "minOs": "10.0.19041",
        "artifacts": [
            {
                "platform": PLATFORM,
                "archive": "tessera-{}-{}.zip".format(version, PLATFORM),
                "sha256": "a" * 64,
                "size": 1024,
                "symbols": {
                    "artifact": "tessera-{}-{}.symbols.txt".format(version, PLATFORM),
                    "sha256": "b" * 64,
                    "size": 4096,
                    "format": model.SYMBOLS_FORMAT,
                    "build": COMMIT,
                },
            }
        ],
        "deprecations": deprecations or [],
    }


def ledger_with(*releases: Dict[str, Any]) -> Dict[str, Any]:
    return {"ledgerVersion": 1, "releases": list(releases)}


class Repository:
    """A throwaway repo: channels, ledger, manifests and a CMakeLists with the project version."""

    def __init__(self, root: Path) -> None:
        self.root = root
        (root / "release" / "manifests").mkdir(parents=True)
        (root / "CMakeLists.txt").write_text(
            "project(Tessera VERSION {})\n".format(VERSION), encoding="utf-8"
        )
        self.write_channels(CHANNELS)
        self.write_ledger(ledger_with())

    def path(self, *parts: str) -> Path:
        return self.root.joinpath(*parts)

    def write_channels(self, channels: Dict[str, Any]) -> None:
        self.path("release", "channels.json").write_text(
            json.dumps(channels, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline=""
        )

    def write_ledger(self, ledger: Dict[str, Any]) -> None:
        self.path("release", "ledger.json").write_text(
            json.dumps(ledger, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline=""
        )

    def read_ledger(self) -> Dict[str, Any]:
        return json.loads(self.path("release", "ledger.json").read_text(encoding="utf-8"))

    def manifest(self, channel: str = "beta", platform: str = PLATFORM) -> Path:
        return self.path("release", "manifests", "{}-{}.json".format(channel, platform))

    def run(self, script: Path, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(script),
                "--channels",
                str(self.path("release", "channels.json")),
                "--ledger",
                str(self.path("release", "ledger.json")),
                "--manifests",
                str(self.path("release", "manifests")),
                "--cmake-lists",
                str(self.path("CMakeLists.txt")),
                *args,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=str(REPO_ROOT),
        )


class TempRepositoryCase(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.repo = Repository(Path(self._temp.name) / "repo")

    def tearDown(self) -> None:
        self._temp.cleanup()

    def record(self, *extra: str, version: str = VERSION) -> subprocess.CompletedProcess:
        archive = write_archive(
            self.repo.path("tessera-{}-{}.zip".format(version, PLATFORM)), package_info(version)
        )
        symbols = self.repo.path("tessera-{}-{}.symbols.txt".format(version, PLATFORM))
        symbols.write_text(symbols_text(), encoding="utf-8")
        return self.repo.run(
            GENERATOR,
            "record",
            "--archive",
            str(archive),
            "--symbols",
            str(symbols),
            "--channel",
            "beta",
            "--date",
            "2026-11-16",
            *extra,
        )
