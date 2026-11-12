#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dependency and third-party notice release gate (ADR-012, ADR-013).

Reads the canonical dependency table in ``docs/specs/dependencies.md`` and the notice file
``THIRD_PARTY_NOTICES.md`` and enforces the license policy of the rendering production plan
(section 5/6):

* every *production-linked* row (``V1 hard``, ``V1 optional``, ``V1 optional alternative``,
  ``V1 transitive``) must use a permissive, allowlisted license;
* GPL/LGPL/AGPL licenses are only accepted on rows that are explicitly build-time, CI,
  test/debug, system, not-linked or rejected;
* every production-linked component needs a heading entry or a planned-notices row;
* every row must carry a known status, and duplicate rows must not conflict.

Only the Python standard library is used, the output is deterministic and no network access is
performed, so the gate runs identically on every OS.

Exit codes: 0 = gate passed, 1 = gate violations, 2 = usage/parse error.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEPENDENCIES = REPO_ROOT / "docs" / "specs" / "dependencies.md"
DEFAULT_NOTICES = REPO_ROOT / "THIRD_PARTY_NOTICES.md"

# Canonical allowlist from docs/design/rendering-production-plan.md section 5. "Public domain"
# and "MIT-0" are added because docs/specs/dependencies.md ("License policy") explicitly allows
# public-domain code in the shipped core; both are strictly permissive.
ALLOWED_LICENSES = frozenset(
    {
        "MIT",
        "MIT-0",
        "BSD-2-Clause",
        "BSD-3-Clause",
        "Apache-2.0",
        "FTL",
        "Unicode",
        "zlib",
        "libpng",
        "IJG",
        "ISC",
        "curl",
        "BSL-1.0",
        "OFL",
        "Public domain",
    }
)

# Status cores -> gate family. The core is the status text with parenthetical qualifiers and
# links removed, lowercased (e.g. "V1 hard (vendored)" -> "v1 hard").
STATUS_FAMILY: Dict[str, str] = {
    # Shipped or shippable runtime dependencies: license allowlist + notice completeness.
    "v1 hard": "production",
    "v1 optional": "production",
    "v1 optional alternative": "production",
    "v1 transitive": "production",
    "v1 target": "production",  # owner-approved V1 target (dependencies.md, ThorVG)
    "android v1 optional": "production",
    # Build/CI/test/debug tooling: never linked into a shipped binary.
    "v1 test": "tooling",
    "v1 ci": "tooling",
    "v1 debug": "tooling",
    "v1 build": "tooling",
    "v1 build-time": "tooling",
    "v1 optional build": "tooling",
    "v1 tool config": "tooling",
    "build-time": "tooling",
    # Design-time only (Wasm/browser toolchain), never in the native link line.
    "v1 hard for design-time": "design-time",
    "v1 design-time": "design-time",
    "design-time tooling only": "tooling",
    # Platform APIs and OS system components: dynamic by necessity, not third-party code.
    "v1": "platform",
    "system": "platform",
    "os": "platform",
    # Not shipped yet / not selected; no notice requirement applies.
    "candidate": "pending",
    "candidate alternative": "pending",
    "fallback only": "pending",  # explicitly not selected (dependencies.md, Skia Graphite)
    "poc": "pending",
    "deferred": "pending",
    "future": "pending",
    "rejected": "pending",
    "not required": "pending",
}

# Module words that make a row tooling even when its status says V1 hard/optional (for example
# "Ninja | V1 optional | ... | build").
TOOLING_MODULE_WORDS = frozenset(
    {"build", "ci", "dev", "repo", "script", "scripts", "github", "test", "tests",
     "benchmark", "benchmarks", "fuzz"}
)

# Components that are not third-party libraries and therefore carry no license cell.
NON_THIRD_PARTY = frozenset({"c++23 standard library"})

# Signals that a GPL/LGPL row is deliberately not linked into the shipped binary.
GPL_EXEMPT_ROLE_MARKERS = (
    "build-time only",
    "never linked",
    "not linked",
    "separate program",
    "separate build program",
    "not shipped",
)
GPL_EXEMPT_LICENSE_MARKERS = ("system", "not linked", "never linked")
GPL_EXEMPT_FAMILIES = frozenset({"tooling", "platform", "design-time"})
GPL_EXEMPT_STATUS_CORES = frozenset({"rejected", "not required"})

GPL_RE = re.compile(r"(?<![a-z])(?:a?gpl|lgpl)(?![a-z])", re.IGNORECASE)

# License spellings used by the dependency tables and the notices file -> canonical name.
LICENSE_ALIASES: Dict[str, str] = {
    "mit": "MIT",
    "mit license": "MIT",
    "mit/x11": "MIT",
    "x11": "MIT",
    "mit-0": "MIT-0",
    "mit0": "MIT-0",
    "bsd-2-clause": "BSD-2-Clause",
    "bsd-2": "BSD-2-Clause",
    "bsd 2 clause": "BSD-2-Clause",
    "bsd-3-clause": "BSD-3-Clause",
    "bsd-3": "BSD-3-Clause",
    "bsd 3 clause": "BSD-3-Clause",
    "apache-2.0": "Apache-2.0",
    "apache 2.0": "Apache-2.0",
    "freetype license": "FTL",
    "freetype license bsd-style": "FTL",
    "ftl": "FTL",
    "unicode license": "Unicode",
    "unicode license v3": "Unicode",
    "unicode": "Unicode",
    "zlib": "zlib",
    "zlib license": "zlib",
    "libpng": "libpng",
    "libpng license": "libpng",
    "ijg": "IJG",
    "ijg license": "IJG",
    "isc": "ISC",
    "curl": "curl",
    "curl license": "curl",
    "bsl-1.0": "BSL-1.0",
    "boost software license 1.0": "BSL-1.0",
    "ofl-1.1": "OFL",
    "sil open font license 1.1": "OFL",
    "ofl": "OFL",
    "public domain": "Public domain",
    "public-domain": "Public domain",
    "publicdomain": "Public domain",
    "psf": "PSF",
    "system": "System",
    "os": "OS",
}

NOTICE_NAME_STOPWORDS = frozenset(
    {"the", "and", "of", "for", "in", "on", "with", "without", "only", "test", "tests",
     "library", "libraries", "component", "components", "sdk", "api", "system"}
)


@dataclass(frozen=True)
class StatusInfo:
    core: str
    family: str
    qualifiers: Tuple[str, ...]


@dataclass(frozen=True)
class Row:
    name: str
    status: str
    license: str
    module: str
    role: str
    section: str
    line: int


@dataclass(frozen=True)
class Violation:
    kind: str
    component: str
    message: str

    def as_dict(self) -> Dict[str, str]:
        return {"kind": self.kind, "component": self.component, "message": self.message}


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def strip_markdown(text: str) -> str:
    text = text.replace("`", "")
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    return text


def strip_parentheticals(text: str) -> Tuple[str, List[str]]:
    qualifiers: List[str] = []
    while True:
        match = re.search(r"\(([^()]*)\)", text)
        if not match:
            break
        qualifiers.append(match.group(1))
        text = text[: match.start()] + " " + text[match.end() :]
    return text, qualifiers


def parse_status(raw: str) -> StatusInfo:
    text = strip_markdown(raw)
    base, qualifiers = strip_parentheticals(text)
    base = re.sub(r"\s+", " ", base).strip().strip(".")
    base = base.split("/")[0].strip().lower()
    if not base:
        return StatusInfo("", "unknown", tuple(qualifiers))
    family = STATUS_FAMILY.get(base)
    if family is None:
        return StatusInfo(base, "unknown", tuple(qualifiers))
    qualifier_text = " ".join(qualifiers).lower()
    if "design-time" in qualifier_text:
        family = "design-time"
    elif "toolchain" in qualifier_text or "tool config" in qualifier_text:
        family = "tooling"
    elif re.search(r"\b(ci|debug|test|tests)\b", qualifier_text):
        family = "tooling"
    return StatusInfo(base, family, tuple(qualifiers))


def module_is_tooling(raw: str) -> bool:
    text = strip_markdown(raw).lower()
    words = set(re.findall(r"[a-z0-9]+", text))
    return bool(words & TOOLING_MODULE_WORDS)


def clean_license_token(raw: str) -> str:
    text = strip_markdown(raw)
    text, _ = strip_parentheticals(text)
    text = text.strip().strip(".").strip()
    text = re.sub(r"\s+", " ", text).lower()
    return text


def split_license_cell(raw: str) -> List[Tuple[str, Optional[str]]]:
    """Return (raw token, canonical license or None) pairs for a license cell."""
    text = strip_markdown(raw).strip().strip(".").strip()
    if not text or text in {"-", "\u2014"}:
        return []
    canonical = LICENSE_ALIASES.get(clean_license_token(text))
    if canonical is not None:
        return [(text, canonical)]
    parts = re.split(r"\s*/\s*|\s*;\s*|\s+or\s+", text)
    tokens: List[Tuple[str, Optional[str]]] = []
    for part in parts:
        part = part.strip()
        if not part:
            continue
        cleaned = clean_license_token(part)
        tokens.append((part, LICENSE_ALIASES.get(cleaned)))
    return tokens


def cells(line: str) -> List[str]:
    body = line.strip()
    if body.startswith("|"):
        body = body[1:]
    if body.endswith("|"):
        body = body[:-1]
    return [part.replace("\\|", "|").strip() for part in re.split(r"(?<!\\)\|", body)]


def is_separator(parts: Sequence[str]) -> bool:
    return bool(parts) and all(re.fullmatch(r":?-{2,}:?", part) for part in parts)


def iter_tables(lines: Sequence[str]) -> Iterable[Tuple[int, List[str], List[Tuple[int, List[str]]]]]:
    index = 0
    while index < len(lines):
        if lines[index].lstrip().startswith("|") and index + 1 < len(lines):
            header = cells(lines[index])
            separator = cells(lines[index + 1])
            if is_separator(separator):
                rows: List[Tuple[int, List[str]]] = []
                cursor = index + 2
                while cursor < len(lines) and lines[cursor].lstrip().startswith("|"):
                    rows.append((cursor + 1, cells(lines[cursor])))
                    cursor += 1
                yield index + 1, header, rows
                index = cursor
                continue
        index += 1


def section_map(lines: Sequence[str]) -> List[str]:
    sections: List[str] = []
    current = ""
    for line in lines:
        match = re.match(r"^#{2,6}\s+(.*)$", line)
        if match:
            current = match.group(1).strip()
        sections.append(current)
    return sections


def parse_dependency_rows(text: str) -> List[Row]:
    lines = text.splitlines()
    sections = section_map(lines)
    rows: List[Row] = []
    for header_line, header, table_rows in iter_tables(lines):
        lower = [name.lower() for name in header]
        if "status" not in lower or "license" not in lower:
            continue
        status_index = lower.index("status")
        license_index = lower.index("license")
        module_index = lower.index("module") if "module" in lower else None
        role_index = lower.index("role in tessera") if "role in tessera" in lower else None
        for line_number, parts in table_rows:
            if len(parts) <= max(status_index, license_index):
                continue
            name = parts[0].strip()
            if not name or name in {"-", "\u2014"}:
                continue
            rows.append(
                Row(
                    name=name,
                    status=parts[status_index].strip(),
                    license=parts[license_index].strip(),
                    module=parts[module_index].strip() if module_index is not None else "",
                    role=parts[role_index].strip() if role_index is not None else "",
                    section=sections[header_line - 1] if header_line - 1 < len(sections) else "",
                    line=line_number,
                )
            )
    return rows


def name_tokens(name: str) -> List[str]:
    text = strip_markdown(name).lower()
    tokens = re.findall(r"[a-z0-9]+", text)
    return [token for token in tokens if token not in NOTICE_NAME_STOPWORDS and len(token) >= 2]


def parse_planned_notice_rows(text: str) -> List[str]:
    """Component names from the "Planned V1 dependencies" table only."""
    rows: List[str] = []
    lines = text.splitlines()
    section = ""
    pending_header: Optional[List[str]] = None
    in_table = False
    for line in lines:
        heading = re.match(r"^(#{2,6})\s+(.*)$", line)
        if heading:
            level = len(heading.group(1))
            if level == 2:
                section = heading.group(2).strip()
            in_table = False
            pending_header = None
            continue
        if line.lstrip().startswith("|"):
            if not in_table:
                pending_header = cells(line)
                in_table = True
                continue
            if is_separator(cells(line)):
                continue
            if section.lower().startswith("planned") and pending_header:
                parts = cells(line)
                if parts and parts[0].strip() and parts[0].strip() not in {"-", "\u2014"}:
                    rows.append(parts[0].strip())
            continue
        in_table = False
        pending_header = None
    return rows


def parse_notice_entries(text: str) -> List[str]:
    """Collect component headings and the rows of the "Planned V1 dependencies" table."""
    entries: List[str] = []
    for line in text.splitlines():
        heading = re.match(r"^(#{3,6})\s+(.*)$", line)
        if heading:
            entries.append(heading.group(2).strip())
    entries.extend(parse_planned_notice_rows(text))
    return entries


def notice_matches(component: str, entries: Sequence[str]) -> Optional[str]:
    component_tokens = set(name_tokens(component))
    component_norm = " ".join(name_tokens(component))
    if not component_tokens:
        return None
    best: Optional[str] = None
    for entry in entries:
        entry_tokens = set(name_tokens(entry))
        if not entry_tokens:
            continue
        entry_norm = " ".join(name_tokens(entry))
        if component_norm and component_norm == entry_norm:
            return entry
        if component_norm and (
            (len(component_norm) >= 3 and component_norm in entry_norm)
            or (len(entry_norm) >= 3 and entry_norm in component_norm)
        ):
            return entry
        if component_tokens & entry_tokens:
            if best is None:
                best = entry
    return best


def is_gpl_row(raw_license: str) -> bool:
    return bool(GPL_RE.search(raw_license))


def gpl_exempt(row: Row, status: StatusInfo) -> bool:
    if status.family in GPL_EXEMPT_FAMILIES or status.core in GPL_EXEMPT_STATUS_CORES:
        return True
    role = row.role.lower()
    if any(marker in role for marker in GPL_EXEMPT_ROLE_MARKERS):
        return True
    license_text = row.license.lower()
    if any(marker in license_text for marker in GPL_EXEMPT_LICENSE_MARKERS):
        return True
    return False


def check(
    dependencies_text: str, notices_text: str
) -> Tuple[List[Violation], List[str], int]:
    rows = parse_dependency_rows(dependencies_text)
    entries = parse_notice_entries(notices_text)
    violations: List[Violation] = []
    warnings: List[str] = []
    seen: Dict[str, Row] = {}
    production_linked = 0

    # Extras in the notices file are reported, never failed: the notices file may legitimately
    # list planned or Phase 0 components that the dependency map does not carry as rows.
    dependency_names = [row.name for row in rows]
    for notice_row in parse_planned_notice_rows(notices_text):
        if notice_matches(notice_row, dependency_names) is None:
            warnings.append(
                "planned-notices row {!r} does not match a dependency-table row "
                "(warning only)".format(notice_row)
            )

    for row in rows:
        status = parse_status(row.status)
        if status.family == "unknown":
            violations.append(
                Violation(
                    "UNKNOWN_STATUS",
                    row.name,
                    "unknown status {!r} (line {}); add it to STATUS_FAMILY or fix the row".format(
                        row.status, row.line
                    ),
                )
            )
            continue

        normalized = " ".join(name_tokens(row.name)) or row.name.lower()
        previous = seen.get(normalized)
        if previous is not None:
            same = (
                parse_status(previous.status).core == status.core
                and previous.license.strip().lower() == row.license.strip().lower()
            )
            if same:
                warnings.append(
                    "duplicate row for {!r} (lines {} and {})".format(
                        row.name, previous.line, row.line
                    )
                )
            else:
                violations.append(
                    Violation(
                        "DUPLICATE_CONFLICT",
                        row.name,
                        "conflicting duplicate rows (line {}: status {!r}, license {!r}; "
                        "line {}: status {!r}, license {!r})".format(
                            previous.line,
                            previous.status,
                            previous.license,
                            row.line,
                            row.status,
                            row.license,
                        ),
                    )
                )
            continue
        seen[normalized] = row

        family = status.family
        if family in {"production", "platform"} and module_is_tooling(row.module):
            family = "tooling"

        if is_gpl_row(row.license):
            if not gpl_exempt(row, status):
                violations.append(
                    Violation(
                        "FORBIDDEN_LICENSE",
                        row.name,
                        "GPL/LGPL/AGPL license {!r} on a row that is not explicitly "
                        "build-time/CI/not-linked/system (status {!r})".format(
                            row.license, row.status
                        ),
                    )
                )
                continue

        if family != "production":
            continue

        production_linked += 1
        tokens = split_license_cell(row.license)
        if not tokens:
            if row.name.strip().lower() in NON_THIRD_PARTY:
                warnings.append(
                    "no license recorded for {!r}; treated as a non-third-party "
                    "standard-library row".format(row.name)
                )
            else:
                violations.append(
                    Violation(
                        "MISSING_LICENSE",
                        row.name,
                        "production-linked row has no license (status {!r})".format(row.status),
                    )
                )
            continue

        for raw_token, canonical in tokens:
            if canonical is None:
                violations.append(
                    Violation(
                        "LICENSE_NOT_ALLOWED",
                        row.name,
                        "license {!r} is not in the shipped-code allowlist "
                        "(status {!r})".format(raw_token, row.status),
                    )
                )
            elif canonical not in ALLOWED_LICENSES:
                violations.append(
                    Violation(
                        "LICENSE_NOT_ALLOWED",
                        row.name,
                        "license {!r} is not allowed for shipped code (status {!r})".format(
                            canonical, row.status
                        ),
                    )
                )

        if notice_matches(row.name, entries) is None:
            violations.append(
                Violation(
                    "MISSING_NOTICE",
                    row.name,
                    "production-linked component (status {!r}) has no heading entry and no "
                    "planned-notices row in THIRD_PARTY_NOTICES.md".format(row.status),
                )
            )

    violations.sort(key=lambda item: (item.kind, item.component, item.message))
    warnings.sort()
    return violations, warnings, production_linked


def format_report(
    dependencies_path: Path,
    notices_path: Path,
    row_count: int,
    production_linked: int,
    violations: Sequence[Violation],
    warnings: Sequence[str],
) -> str:
    lines = [
        "Dependency/license gate: {} ({} rows, {} production-linked)".format(
            dependencies_path, row_count, production_linked
        ),
        "Notices: {}".format(notices_path),
    ]
    for warning in warnings:
        lines.append("WARN  {}".format(warning))
    for violation in violations:
        lines.append(
            "ERROR [{}] {}: {}".format(violation.kind, violation.component, violation.message)
        )
    if violations:
        lines.append("FAIL: {} violation(s).".format(len(violations)))
    else:
        lines.append("PASS: licenses, statuses and notices are consistent.")
    return "\n".join(lines)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--dependencies",
        default=str(DEFAULT_DEPENDENCIES),
        help="dependency table markdown (default: docs/specs/dependencies.md)",
    )
    parser.add_argument(
        "--notices",
        default=str(DEFAULT_NOTICES),
        help="third-party notices markdown (default: THIRD_PARTY_NOTICES.md)",
    )
    parser.add_argument("--json", action="store_true", dest="as_json", help="machine-readable report")
    args = parser.parse_args(argv)

    dependencies_path = Path(args.dependencies)
    notices_path = Path(args.notices)
    missing = [path for path in (dependencies_path, notices_path) if not path.is_file()]
    if missing:
        for path in missing:
            print("error: file not found: {}".format(path), file=sys.stderr)
        return 2

    dependencies_text = read_text(dependencies_path)
    notices_text = read_text(notices_path)
    rows = parse_dependency_rows(dependencies_text)
    violations, warnings, production_linked = check(dependencies_text, notices_text)
    row_count = len(rows)

    if args.as_json:
        payload = {
            "ok": not violations,
            "gate": "dependency-license",
            "dependency_file": str(dependencies_path),
            "notices_file": str(notices_path),
            "row_count": row_count,
            "production_linked": production_linked,
            "violations": [violation.as_dict() for violation in violations],
            "warnings": list(warnings),
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print(
            format_report(
                dependencies_path,
                notices_path,
                row_count,
                production_linked,
                violations,
                warnings,
            )
        )
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
