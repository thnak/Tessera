#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Surface extraction shared by the versioning gate (issue #194).

Three versioned surfaces are read straight out of the tree, so the gate needs no build, no network
and no git:

* **RHI** -- the public C++ contract in ``include/tessera/rendering/rhi/**``. Its version is the
  pair ``kRhiVersionMajor``/``kRhiVersionMinor`` in ``version.hpp`` (ADR-049). The surface is a
  digest of the contract headers with comments and blank lines removed, so an edit to a doc comment
  does not force a version move while any declaration change does.
* **IR** -- the schema tables of ``docs/specs/ir-v1.md`` and ``docs/specs/ir-v1-expr.md`` (the
  normative field tables). Its version is ``kIrVersion`` in the IR module header.
* **``tessera.json``** -- the schema tables of ``docs/specs/project-config-v1.md`` plus the
  ``runtime`` and ``diagnostics`` objects, which have their own specs. Its version is the accepted
  value the harness loader states, ``schemaVersion must be N``.

The IR and ``tessera.json`` surfaces are field maps, not digests, because those two schemas allow
additive changes to keep the version (``ir-v1.md`` section 12, ``project-config-v1.md`` Versioning):
the gate has to tell an added optional field from a removed or retyped one.

Nothing here decides policy; :mod:`check_version_policy` does. This module only answers "what is the
surface and what is its version" deterministically: every collection is sorted and no clock, locale,
environment or random value is consulted.
"""

from __future__ import annotations

import hashlib
import re
from pathlib import Path
from typing import Dict, Iterator, List, Sequence, Tuple

# --- the input sets, named once ---------------------------------------------------------------

RHI_DIR = "include/tessera/rendering/rhi"
RHI_VERSION_HEADER = RHI_DIR + "/version.hpp"
IR_VERSION_HEADER = "src/compiler/ir/include/tessera/compiler/ir/module.hpp"
TESSERA_JSON_LOADER = "src/harness/manifest_loader.cpp"

IR_SPECS: Tuple[str, ...] = ("docs/specs/ir-v1.md", "docs/specs/ir-v1-expr.md")
TESSERA_JSON_SPECS: Tuple[str, ...] = (
    "docs/specs/project-config-v1.md",
    "docs/specs/runtime-config-v1.md",
    "docs/specs/diagnostics-config-v1.md",
)

BASELINE = "scripts/versioning-baseline.txt"

# Surface name -> the spec file a change is reported against when the field itself has no source
# (a version bump, or a whole-surface failure).
SURFACE_ANCHOR: Dict[str, str] = {
    "rhi": RHI_VERSION_HEADER,
    "ir": IR_SPECS[0],
    "tessera.json": TESSERA_JSON_SPECS[0],
}

_HEADING_RE = re.compile(r"^##\s+(?P<title>.+?)\s*$")
_SEPARATOR_RE = re.compile(r"^\|[\s:|-]+\|$")
_RHI_VERSION_RE = {
    "major": re.compile(r"kRhiVersionMajor\s*=\s*(\d+)"),
    "minor": re.compile(r"kRhiVersionMinor\s*=\s*(\d+)"),
}
_IR_VERSION_RE = re.compile(r"kIrVersion\s*=\s*(\d+)")
_TESSERA_JSON_VERSION_RE = re.compile(r"schemaVersion must be (\d+)")


class SurfaceError(Exception):
    """A surface input is missing or no longer states its version."""


# --- small text helpers -----------------------------------------------------------------------


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise SurfaceError("missing input file: {}".format(path.as_posix()))
    except UnicodeDecodeError:
        # A surface input that is not UTF-8 is a broken repository, not a comparison: report it as
        # an unreadable surface instead of letting a decode error escape as a traceback.
        raise SurfaceError("input file is not valid UTF-8: {}".format(path.as_posix()))
    except OSError as error:
        raise SurfaceError("cannot read {}: {}".format(path.as_posix(), error))


def normalize_cell(text: str) -> str:
    """One markdown cell, comparable across cosmetic edits.

    Backticks, the emphasis they carry and repeated spaces are presentation; a type is ``string``
    whether the table writes ``string`` or ``` `string` ```.
    """
    cell = " ".join(text.split())
    while len(cell) >= 2 and cell.startswith("`") and cell.endswith("`"):
        cell = cell[1:-1].strip()
    return " ".join(cell.split())


def is_required(cell: str) -> bool:
    """``Required``/``Required?`` cells say ``yes ...`` when the field must be present."""
    return bool(re.match(r"(?i)^yes\b", normalize_cell(cell)))


def split_row(line: str) -> List[str]:
    stripped = line.strip()
    if stripped.startswith("|"):
        stripped = stripped[1:]
    if stripped.endswith("|"):
        stripped = stripped[:-1]
    return [normalize_cell(cell) for cell in stripped.split("|")]


def normalize_source(text: str) -> str:
    """C++ contract text with comments and blank lines removed, for the RHI digest.

    A block comment is dropped while it is open; a line that merely contains ``/* ... */`` before
    code keeps its code because only a line *starting* with the comment is dropped. The point is
    narrow: a behaviour note above a declaration is documentation of an existing contract, while the
    declaration itself is the contract.
    """
    lines: List[str] = []
    depth = 0
    for raw in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        stripped = raw.strip()
        if depth:
            if "*/" in stripped:
                depth = 0
            continue
        if stripped.startswith("/*"):
            if "*/" not in stripped:
                depth = 1
            continue
        if not stripped or stripped.startswith("//"):
            continue
        lines.append(raw.rstrip())
    return "\n".join(lines)


# --- markdown schema tables -------------------------------------------------------------------


def parse_tables(text: str) -> Iterator[Tuple[str, List[str], List[List[str]]]]:
    """Yield ``(section, header cells, body rows)`` for every ``Field | Type`` table.

    Only tables that declare a ``Field`` and a ``Type`` column are schema tables; the many
    ``Topic | Rule`` and ``Kind | Shape`` tables in the specs are prose and are skipped. A section
    with more than one schema table disambiguates them in document order (``#2``), so table order is
    part of the key: moving a table between sections is a surface change, which is correct.
    """
    section = ""
    lines = text.replace("\r\n", "\n").split("\n")
    index = 0
    seen: Dict[str, int] = {}
    while index < len(lines):
        line = lines[index]
        heading = _HEADING_RE.match(line)
        if heading:
            section = heading.group("title")
            index += 1
            continue
        is_header = (
            line.strip().startswith("|")
            and index + 1 < len(lines)
            and _SEPARATOR_RE.match(lines[index + 1].strip()) is not None
        )
        if not is_header:
            index += 1
            continue
        header = split_row(line)
        if "Field" not in header or "Type" not in header:
            index += 2
            continue
        rows: List[List[str]] = []
        index += 2
        while index < len(lines) and lines[index].strip().startswith("|"):
            rows.append(split_row(lines[index]))
            index += 1
        count = seen.get(section, 0) + 1
        seen[section] = count
        label = section if count == 1 else "{}#{}".format(section, count)
        yield label, header, rows


def extract_fields(root: Path, specs: Sequence[str]) -> Dict[str, Dict[str, str]]:
    """Field map for one schema surface: ``"<spec>::<table>::<field>" -> {type, required, default,
    source}``.

    The key starts with the spec file stem so two specs that happen to share a section title cannot
    collide, and so a field that was removed still resolves to the spec that used to declare it
    (:func:`source_of`).
    """
    fields: Dict[str, Dict[str, str]] = {}
    for spec in specs:
        text = read_text(root / spec)
        stem = Path(spec).stem
        for table, header, rows in parse_tables(text):
            column = {name: position for position, name in enumerate(header)}
            for row in rows:
                if len(row) != len(header):
                    continue
                name = row[column["Field"]]
                if not name:
                    continue
                entry = {"type": row[column["Type"]], "source": spec}
                # "-" is the recorded value for "this table has no such column", so the baseline
                # line and the tree always compare like with like.
                entry["required"] = (
                    "yes" if is_required(row[column["Required"]]) else "no"
                ) if "Required" in column else "-"
                entry["default"] = row[column["Default"]] if "Default" in column else "-"
                fields["{}::{}::{}".format(stem, table, name)] = entry
    return fields


def source_of(key: str) -> str:
    """The spec file a field key belongs to, including a field that is no longer declared.

    A key is ``<spec stem>::<table>::<field>``, so the stem alone names the file. An unrecognised
    stem (a spec renamed or dropped since the recording) is reported as the stem, which is enough
    for the author to find it.
    """
    stem = key.split("::", 1)[0]
    for spec in IR_SPECS + TESSERA_JSON_SPECS:
        if Path(spec).stem == stem:
            return spec
    return stem + " (spec no longer present)"


# --- the three surfaces -----------------------------------------------------------------------


def rhi_contract_files(root: Path) -> List[str]:
    """Every public RHI header except ``version.hpp`` (whose text is the version, not the contract)."""
    directory = root / RHI_DIR
    if not directory.is_dir():
        raise SurfaceError("missing RHI contract directory: {}".format(RHI_DIR))
    return sorted(
        path.relative_to(root).as_posix()
        for path in directory.glob("*.hpp")
        if path.name != "version.hpp"
    )


def rhi_digest(root: Path) -> str:
    digest = hashlib.sha256()
    files = rhi_contract_files(root)
    if not files:
        raise SurfaceError("no RHI contract headers under {}".format(RHI_DIR))
    for relative in files:
        text = normalize_source(read_text(root / relative))
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(text.encode("utf-8"))
        digest.update(b"\0")
    return digest.hexdigest()


def rhi_version(root: Path) -> Tuple[int, int]:
    text = read_text(root / RHI_VERSION_HEADER)
    major = _RHI_VERSION_RE["major"].search(text)
    minor = _RHI_VERSION_RE["minor"].search(text)
    if major is None or minor is None:
        raise SurfaceError(
            "{} no longer defines kRhiVersionMajor and kRhiVersionMinor".format(RHI_VERSION_HEADER)
        )
    return int(major.group(1)), int(minor.group(1))


def ir_version(root: Path) -> int:
    match = _IR_VERSION_RE.search(read_text(root / IR_VERSION_HEADER))
    if match is None:
        raise SurfaceError("{} no longer defines kIrVersion".format(IR_VERSION_HEADER))
    return int(match.group(1))


def tessera_json_version(root: Path) -> int:
    match = _TESSERA_JSON_VERSION_RE.search(read_text(root / TESSERA_JSON_LOADER))
    if match is None:
        raise SurfaceError(
            "{} no longer states the accepted schemaVersion "
            "(expected a 'schemaVersion must be N' diagnostic)".format(TESSERA_JSON_LOADER)
        )
    return int(match.group(1))


def build_surfaces(root: Path) -> Dict[str, Dict[str, object]]:
    """The current state of all three surfaces, keyed by surface name."""
    major, minor = rhi_version(root)
    return {
        "ir": {"version": ir_version(root), "fields": extract_fields(root, IR_SPECS)},
        "rhi": {"version": [major, minor], "digest": rhi_digest(root)},
        "tessera.json": {
            "version": tessera_json_version(root),
            "fields": extract_fields(root, TESSERA_JSON_SPECS),
        },
    }
