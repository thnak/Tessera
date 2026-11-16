#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Filesystem helpers for the release-manifest tooling (issue #193).

Reading and writing the three documents, and nothing else: the rules live in the ``release_*_rules``
modules, the model in :mod:`release_manifest`. Split out so the generator and the checker share one
implementation of "what is on disk" and cannot disagree.

The write path is a **diff**, not a rewrite: a file whose bytes would not change is left alone (so a
rerun touches no mtime and produces no diff), a file the ledger projects is written canonical, and a
file the ledger no longer projects is deleted with a printed reason. ``--check`` performs the same
walk and reports instead of writing.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

import release_manifest as model


@dataclass(frozen=True)
class Paths:
    channels: Path
    ledger: Path
    manifests: Path
    cmake: Path


def rel(path: Path) -> str:
    """A repository-relative POSIX path for messages, or the path as given when it is outside."""
    try:
        return path.resolve().relative_to(model_repo_root()).as_posix()
    except (ValueError, OSError):
        return str(path)


def model_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def project_version(cmake_path: Path) -> Optional[str]:
    """``project(Tessera VERSION ...)`` from the root CMakeLists, or ``None`` when unreadable."""
    try:
        text = cmake_path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return None
    import release_policy as policy

    return policy.project_version_from_cmake(text)


def read_committed(manifests_dir: Path) -> Dict[str, str]:
    """``{file name: text}`` for every committed manifest, in sorted order."""
    if not manifests_dir.is_dir():
        return {}
    committed: Dict[str, str] = {}
    for path in sorted(manifests_dir.glob("*.json")):
        if not path.is_file():
            continue
        try:
            committed[path.name] = path.read_text(encoding="utf-8")
        except UnicodeDecodeError as error:
            raise model.ManifestError("{}: not valid UTF-8: {}".format(path, error))
    return committed


def write_projection(
    manifests_dir: Path, projection: Dict[str, Dict[str, Any]], *, check: bool, quiet: bool
) -> int:
    """Write (or with ``check`` only report) the projected manifests.

    Returns 0 when the tree matches the projection, 1 when ``--check`` found drift. A write run
    prints one line per changed file so the diff is visible in the command's own output.
    """
    committed = read_committed(manifests_dir)
    written: List[str] = []
    removed: List[str] = []
    drift: List[str] = []

    for name, document in sorted(projection.items()):
        expected = model.render(document)
        if committed.get(name) == expected:
            continue
        drift.append(name)
        if not check:
            model.write_document(manifests_dir / name, document)
            written.append(name)
    for name in sorted(set(committed) - set(projection)):
        drift.append(name)
        if not check:
            (manifests_dir / name).unlink()
            removed.append(name)

    if check:
        for name in drift:
            print(
                "stale: {}/{}".format(rel(manifests_dir), name),
                file=sys.stderr,
            )
        if drift:
            print(
                "error: {} manifest(s) differ from the ledger projection; run "
                "`python -B scripts/gen_release_manifest.py project`".format(len(drift)),
                file=sys.stderr,
            )
            return 1
        if not quiet:
            print(
                "{} manifest(s) match the ledger projection".format(len(projection))
            )
        return 0

    for name in written:
        print("wrote {}/{}".format(rel(manifests_dir), name))
    for name in removed:
        print("removed {}/{} (the ledger no longer projects it)".format(rel(manifests_dir), name))
    if not written and not removed and not quiet:
        print("{} manifest(s) already current; nothing written".format(len(projection)))
    return 0
