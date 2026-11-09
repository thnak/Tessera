#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate and verify the shipped third-party license bundle (ADR-012, ADR-013).

Reads the production-linked rows from ``docs/specs/dependencies.md`` and the component metadata in
``THIRD_PARTY_NOTICES.md``, then renders ``THIRD_PARTY_LICENSES.txt``: a deterministic, sorted
bundle with a component -> license -> copyright map followed by each distinct license text exactly
once (texts live in ``licenses/`` with a provenance header).

Usage (stdlib only, deterministic, runs from any directory):

    python -B scripts/gen_license_bundle.py --write
    python -B scripts/gen_license_bundle.py --check

``--check`` never writes: it exits 1 when the bundle is stale or a production-linked license has no
local text and the copyright cannot be resolved. Exit codes: 0 pass, 1 verification failure,
2 usage/unreadable input.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
DEFAULT_DEPENDENCIES = REPO_ROOT / "docs" / "specs" / "dependencies.md"
DEFAULT_NOTICES = REPO_ROOT / "THIRD_PARTY_NOTICES.md"
DEFAULT_LICENSES_DIR = REPO_ROOT / "licenses"
DEFAULT_OUTPUT = REPO_ROOT / "THIRD_PARTY_LICENSES.txt"

if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import check_dependencies as deps  # noqa: E402  (sibling script, stdlib only)

# Canonical license name (as parsed by check_dependencies) -> text file in licenses/.
LICENSE_FILES: Dict[str, str] = {
    "Apache-2.0": "Apache-2.0.txt",
    "BSD-3-Clause": "BSD-3-Clause.txt",
    "FTL": "FTL.txt",
    "IJG": "IJG.txt",
    "ISC": "ISC.txt",
    "MIT": "MIT.txt",
    "MIT-0": "MIT-0.txt",
    "Public domain": "Public-Domain-SQLite.txt",
    "Unicode": "Unicode-3.0.txt",
    "curl": "curl.txt",
    "libpng": "Libpng.txt",
    "zlib": "Zlib.txt",
}

# Public domain has no canonical license text. SQLite is the only production-linked component that
# is public domain without a permissive alternative; its dedication is quoted in the mapped file.
PUBLIC_DOMAIN_COMPONENTS = frozenset({"sqlite3"})

PROVENANCE_PREFIX = "SPDX-License-Identifier:"


class BundleError(Exception):
    """A verification or configuration failure that must fail the gate."""


@dataclass(frozen=True)
class Component:
    name: str
    licenses: Tuple[str, ...]
    copyright: str
    status: str = ""


@dataclass(frozen=True)
class NoticeEntry:
    name: str
    source: str  # "in-repo" heading or "planned" table row
    copyright: str = ""
    upstream: str = ""


def clean_text(text: str) -> str:
    return " ".join(deps.strip_markdown(text).split())


def elect_licenses(component: str, tokens: Sequence[str]) -> Tuple[str, ...]:
    """Return the license(s) shipped for a component, electing a permissive dual-license option."""
    elected = list(tokens)
    if "Public domain" in elected and len(elected) > 1:
        # Dual license: ship the permissive alternative instead of a public-domain dedication.
        elected = [token for token in elected if token != "Public domain"]
    if not elected:
        raise BundleError("production-linked component {!r} has no usable license".format(component))
    if elected == ["Public domain"]:
        key = clean_text(component).lower()
        if key not in PUBLIC_DOMAIN_COMPONENTS:
            raise BundleError(
                "no canonical public-domain text for {!r}: public domain has no license text and "
                "the bundle does not invent one; add an authoritative text and update "
                "PUBLIC_DOMAIN_COMPONENTS".format(component)
            )
    return tuple(elected)


def production_components(dependencies_text: str) -> List[Component]:
    components: List[Component] = []
    for row in deps.parse_dependency_rows(dependencies_text):
        status = deps.parse_status(row.status)
        if status.family == "unknown":
            raise BundleError(
                "dependency row {!r} has unknown status {!r}; fix or classify it before "
                "generating the bundle".format(row.name, row.status)
            )
        family = status.family
        if family in {"production", "platform"} and deps.module_is_tooling(row.module):
            family = "tooling"
        if family != "production":
            continue
        if clean_text(row.name).lower() in deps.NON_THIRD_PARTY:
            continue
        tokens = [canonical for _, canonical in deps.split_license_cell(row.license) if canonical]
        if not tokens:
            raise BundleError(
                "production-linked component {!r} has no license in "
                "docs/specs/dependencies.md".format(row.name)
            )
        components.append(
            Component(
                name=clean_text(row.name),
                licenses=elect_licenses(row.name, tokens),
                copyright="",
                status=row.status,
            )
        )
    return components


def parse_notice_entries(notices_text: str) -> List[NoticeEntry]:
    lines = notices_text.splitlines()
    entries: List[NoticeEntry] = []
    section = ""
    current: Optional[Dict[str, str]] = None

    def flush() -> None:
        nonlocal current
        if current is not None:
            entries.append(
                NoticeEntry(
                    name=current.get("name", ""),
                    source="in-repo",
                    copyright=clean_text(current.get("copyright", "")),
                    upstream=clean_text(current.get("upstream", "")),
                )
            )
            current = None

    for line in lines:
        heading = deps.re.match(r"^(#{2,6})\s+(.*)$", line)
        if heading:
            flush()
            level = len(heading.group(1))
            title = heading.group(2).strip()
            if level == 2:
                section = title
            else:
                current = {"name": clean_text(title)}
            continue
        if line.lstrip().startswith("|"):
            flush()
            parts = deps.cells(line)
            lowered = [part.lower() for part in parts]
            if deps.is_separator(parts):
                continue
            if "component" in lowered and "copyright / upstream" in lowered:
                continue
            if (
                section.lower().startswith("planned")
                and len(parts) >= 3
                and parts[0].strip()
                and parts[0].strip() not in {"-", "\u2014"}
            ):
                entries.append(
                    NoticeEntry(
                        name=clean_text(parts[0]),
                        source="planned",
                        copyright=clean_text(parts[2]),
                        upstream=clean_text(parts[2]),
                    )
                )
            continue
        if current is not None:
            bullet = deps.re.match(r"^\s*[-*]\s+([A-Za-z][A-Za-z /]+):\s*(.*)$", line)
            if bullet:
                key = bullet.group(1).strip().lower()
                value = bullet.group(2).strip()
                if key:
                    current[key] = value
            continue
        flush()
    flush()
    return entries


def notice_entry_for(
    component: str,
    entries: Sequence[NoticeEntry],
    require_copyright: bool = False,
) -> Optional[NoticeEntry]:
    """Resolve the notices entry (copyright/upstream metadata) for a component."""
    tokens = set(deps.name_tokens(component))
    candidates: List[Tuple[int, int, int, int, NoticeEntry]] = []
    for index, entry in enumerate(entries):
        if require_copyright and not entry.copyright:
            continue
        entry_tokens = set(deps.name_tokens(entry.name))
        if not entry_tokens:
            continue
        if tokens == entry_tokens:
            score = 0
        elif tokens & entry_tokens:
            score = 1
        else:
            continue
        has_metadata = 0 if (entry.copyright or entry.upstream) else 1
        source_rank = 0 if entry.source == "in-repo" else 1
        candidates.append((has_metadata, source_rank, score, index, entry))
    if not candidates:
        return None
    candidates.sort(key=lambda item: item[:4])
    return candidates[0][4]


def copyright_for(component: str, entries: Sequence[NoticeEntry]) -> str:
    entry = notice_entry_for(component, entries, require_copyright=True)
    if entry is None or not entry.copyright:
        raise BundleError(
            "no copyright holder for production-linked component {!r}; add it to "
            "THIRD_PARTY_NOTICES.md".format(component)
        )
    return entry.copyright


def load_license_text(licenses_dir: Path, license_name: str, components: Sequence[str]) -> str:
    filename = LICENSE_FILES.get(license_name)
    if filename is None:
        raise BundleError(
            "no license text mapping for {!r} (needed by {}); add licenses/<file>.txt and update "
            "LICENSE_FILES".format(license_name, ", ".join(components))
        )
    path = licenses_dir / filename
    if not path.is_file():
        raise BundleError(
            "missing license text {} for {!r} (needed by {})".format(
                path, license_name, ", ".join(components)
            )
        )
    text = path.read_text(encoding="utf-8")
    normalized = text.replace("\r\n", "\n").replace("\r", "\n").strip("\n")
    if not normalized.startswith(PROVENANCE_PREFIX):
        raise BundleError(
            "license text {} must start with a {!r} provenance header".format(
                path, PROVENANCE_PREFIX
            )
        )
    if len(normalized.splitlines()) < 3:
        raise BundleError("license text {} looks empty".format(path))
    return normalized


def render_bundle(
    dependencies_text: str, notices_text: str, licenses_dir: Path
) -> Tuple[str, int, int]:
    components = production_components(dependencies_text)
    entries = parse_notice_entries(notices_text)

    resolved: List[Component] = []
    for component in components:
        resolved.append(
            Component(
                name=component.name,
                licenses=component.licenses,
                copyright=copyright_for(component.name, entries),
            )
        )
    resolved.sort(key=lambda item: (item.name.casefold(), item.name))

    license_users: Dict[str, List[str]] = {}
    for component in resolved:
        for license_name in component.licenses:
            license_users.setdefault(license_name, []).append(component.name)
    license_names = sorted(license_users, key=lambda name: (name.casefold(), name))
    license_texts = [
        (name, load_license_text(licenses_dir, name, license_users[name]))
        for name in license_names
    ]

    lines: List[str] = [
        "THIRD-PARTY LICENSES",
        "====================",
        "",
        "Generated by scripts/gen_license_bundle.py from docs/specs/dependencies.md (production-",
        "linked rows) and THIRD_PARTY_NOTICES.md. Do not edit by hand: regenerate with",
        "`python -B scripts/gen_license_bundle.py --write` and verify with `--check`.",
        "",
        "This bundle lists every third-party component linked into a production Tessera build with",
        "its license and copyright holder, followed by the full text of each distinct license",
        "exactly once. System libraries that stay dynamic (Vulkan loader/ICDs, OS frameworks,",
        "glibc) are not linked third-party code and are not listed here; THIRD_PARTY_NOTICES.md",
        "remains the full component map.",
        "",
        "Where the dependency map lists a dual license, the bundle ships the elected permissive",
        "option (miniaudio: MIT-0, rpmalloc: MIT); libjpeg-turbo ships BSD-3-Clause and IJG.",
        "",
        "Components ({})".format(len(resolved)),
        "----------------",
        "",
    ]
    for component in resolved:
        lines.append(component.name)
        lines.append("  License: {}".format(" / ".join(component.licenses)))
        lines.append("  Copyright: {}".format(component.copyright))
        lines.append("")

    lines.extend(
        [
            "License texts ({})".format(len(license_texts)),
            "------------------",
            "",
        ]
    )
    for license_name, text in license_texts:
        lines.append("-" * 80)
        lines.append("License: {}".format(license_name))
        lines.append("-" * 80)
        lines.append("")
        lines.append(text)
        lines.append("")
    bundle = "\n".join(lines).rstrip("\n") + "\n"
    return bundle, len(resolved), len(license_texts)


def describe_drift(expected: str, actual: str) -> str:
    expected_lines = expected.splitlines()
    actual_lines = actual.splitlines()
    for index in range(max(len(expected_lines), len(actual_lines))):
        expected_line = expected_lines[index] if index < len(expected_lines) else "<end of file>"
        actual_line = actual_lines[index] if index < len(actual_lines) else "<end of file>"
        if expected_line != actual_line:
            return (
                "first difference at line {}:\n  expected: {}\n  found:    {}".format(
                    index + 1, expected_line, actual_line
                )
            )
    return "content differs"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="write THIRD_PARTY_LICENSES.txt")
    mode.add_argument("--check", action="store_true", help="verify the bundle is up to date")
    parser.add_argument("--dependencies", default=str(DEFAULT_DEPENDENCIES))
    parser.add_argument("--notices", default=str(DEFAULT_NOTICES))
    parser.add_argument("--licenses-dir", default=str(DEFAULT_LICENSES_DIR))
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    args = parser.parse_args(argv)

    dependencies_path = Path(args.dependencies)
    notices_path = Path(args.notices)
    licenses_dir = Path(args.licenses_dir)
    output_path = Path(args.output)
    for path in (dependencies_path, notices_path):
        if not path.is_file():
            print("error: file not found: {}".format(path), file=sys.stderr)
            return 2
    if not licenses_dir.is_dir():
        print("error: license text directory not found: {}".format(licenses_dir), file=sys.stderr)
        return 2

    try:
        bundle, component_count, license_count = render_bundle(
            dependencies_path.read_text(encoding="utf-8"),
            notices_path.read_text(encoding="utf-8"),
            licenses_dir,
        )
    except BundleError as error:
        print("error: {}".format(error), file=sys.stderr)
        return 1

    size = len(bundle.encode("utf-8"))
    if args.write:
        output_path.write_text(bundle, encoding="utf-8", newline="")
        print(
            "wrote {} ({} components, {} license texts, {} bytes)".format(
                output_path, component_count, license_count, size
            )
        )
        return 0

    if not output_path.is_file():
        print(
            "error: {} is missing; run `python -B scripts/gen_license_bundle.py --write`".format(
                output_path
            ),
            file=sys.stderr,
        )
        return 1
    actual = output_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    if actual != bundle:
        print(
            "error: {} is stale ({} components, {} license texts, {} bytes expected); "
            "run `python -B scripts/gen_license_bundle.py --write`".format(
                output_path, component_count, license_count, size
            ),
            file=sys.stderr,
        )
        print(describe_drift(bundle, actual), file=sys.stderr)
        return 1
    print(
        "{} is up to date ({} components, {} license texts, {} bytes)".format(
            output_path, component_count, license_count, size
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
