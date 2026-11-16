#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The versioning-policy baseline: the last reviewed state of the three gated surfaces (#194).

One line-oriented text file, not JSON: a reviewer has to be able to see which field moved between
two recordings, and one line per field does that while a nested JSON tree hides it. Every collection
is written from a sorted walk and the file has no clock, path or environment in it, so two runs are
byte-identical.

The format is owned by this module; :mod:`version_surface` produces the payload and
:mod:`check_version_policy` decides what a difference means.
"""

from __future__ import annotations

from pathlib import Path
from typing import Dict, Optional

from version_surface import BASELINE, SurfaceError

# --- baseline file ----------------------------------------------------------------------------
#
# A line-oriented text file, not JSON: a reviewer has to be able to see which field moved between
# two recordings, and one line per field does that while a nested JSON tree hides it. Tabs separate
# the columns, the field key (which contains spaces) is last-but-three and never contains a tab, and
# the whole file is written from a sorted walk, so two runs are byte-identical.

BASELINE_HEADER = """\
# SPDX-License-Identifier: MIT
#
# The reviewed versioning-policy surface (issue #194, docs/specs/versioning-v1.md section 11).
# scripts/check_version_policy.py fails when the tree differs from this record without a version
# move for a breaking change, and `--update` rewrites it after a reviewed change. Do not hand-edit:
# every difference from the tree is an error, including a cosmetic one.
#
# Columns, tab separated: kind, surface, ...payload.
#   version  <surface>  <version>                       one per gated surface
#   digest   rhi        <sha256 of the contract text>
#   field    <surface>  <key>  <type>  <required>  <default>
# A field that has no Required or Default column in its spec table records "-" in that column.
"""


def baseline_path(root: Path) -> Path:
    """Where the recording lives, relative to the repository root."""
    return root / BASELINE


def load_baseline(root: Path) -> Optional[Dict[str, Dict[str, object]]]:
    """The recorded surfaces, or ``None`` when the baseline has never been written."""
    path = baseline_path(root)
    if not path.is_file():
        return None
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise SurfaceError("cannot read {}: {}".format(BASELINE, error))
    surfaces: Dict[str, Dict[str, object]] = {}
    for number, line in enumerate(text.splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        kind = parts[0]
        if kind == "version" and len(parts) == 3:
            entry = surfaces.setdefault(parts[1], {})
            entry["version"] = _parse_version(parts[1], parts[2])
        elif kind == "digest" and len(parts) == 3:
            surfaces.setdefault(parts[1], {})["digest"] = parts[2]
        elif kind == "field" and len(parts) == 6:
            fields = surfaces.setdefault(parts[1], {}).setdefault("fields", {})
            if not isinstance(fields, dict):
                raise SurfaceError("{}:{}: field row after a malformed surface".format(
                    BASELINE, number))
            fields[parts[2]] = {
                "type": parts[3],
                "source": "",
                "required": parts[4],
                "default": parts[5],
            }
        else:
            raise SurfaceError(
                "{}:{}: malformed baseline line: {}".format(BASELINE, number, line[:80])
            )
    return surfaces


def _parse_version(name: str, text: str) -> object:
    if name == "rhi":
        parts = text.split(".")
        if len(parts) != 2 or not all(part.isdigit() for part in parts):
            raise SurfaceError("{}: rhi version `{}` is not MAJOR.MINOR".format(BASELINE, text))
        return [int(parts[0]), int(parts[1])]
    if not text.isdigit():
        raise SurfaceError("{}: {} version `{}` is not an integer".format(BASELINE, name, text))
    return int(text)


def dump_baseline(surfaces: Dict[str, Dict[str, object]]) -> str:
    """Byte-stable serialization: sorted, tab separated, one trailing newline, no clock."""
    lines = [BASELINE_HEADER.rstrip("\n")]
    for name in sorted(surfaces):
        entry = surfaces[name]
        if "version" in entry:
            lines.append("version\t{}\t{}".format(name, _format_version(entry["version"])))
        if "digest" in entry:
            lines.append("digest\t{}\t{}".format(name, entry["digest"]))
    for name in sorted(surfaces):
        fields = surfaces[name].get("fields") or {}
        for key in sorted(fields):
            field = fields[key]
            lines.append(
                "field\t{}\t{}\t{}\t{}\t{}".format(
                    name,
                    key,
                    field.get("type", "-"),
                    field.get("required", "-"),
                    field.get("default", "-"),
                )
            )
    return "\n".join(lines) + "\n"


def _format_version(value: object) -> str:
    if isinstance(value, list):
        return ".".join(str(part) for part in value)
    return str(value)
