#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate and verify the shipped SPDX 2.3 JSON SBOM.

The document describes the project package plus every **production-linked** third-party component
from ``docs/specs/dependencies.md`` (the same rows the license bundle ships). Test, CI, build-time,
design-time and candidate rows are excluded: they are not linked into a shipped binary. Component
metadata (copyright holders, upstream URLs, pinned versions) comes from ``THIRD_PARTY_NOTICES.md``
and the dependency status column, reusing the parsing helpers in ``gen_license_bundle.py``.

The output is byte-deterministic: fixed creation time and tool creator, a content-derived
documentNamespace (no timestamps, no filesystem paths), sorted packages/relationships and fixed
JSON indentation. ``filesAnalyzed`` is false for every package, so no per-file hashes are recorded.

Usage (stdlib only, runs from any directory):

    python -B scripts/gen_sbom.py --write
    python -B scripts/gen_sbom.py --check

Exit codes: 0 pass, 1 drift or missing component data, 2 usage/unreadable input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
DEFAULT_DEPENDENCIES = REPO_ROOT / "docs" / "specs" / "dependencies.md"
DEFAULT_NOTICES = REPO_ROOT / "THIRD_PARTY_NOTICES.md"
DEFAULT_LICENSES_DIR = REPO_ROOT / "licenses"
DEFAULT_OUTPUT = REPO_ROOT / "sbom.spdx.json"
DEFAULT_LICENSE_FILE = REPO_ROOT / "LICENSE"
DEFAULT_CMAKELISTS = REPO_ROOT / "CMakeLists.txt"

if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import gen_license_bundle as bundle  # noqa: E402  (sibling script, stdlib only)

PROJECT_NAME = "Tessera"
PROJECT_DOWNLOAD_LOCATION = "https://github.com/thnak/Tessera"
PROJECT_SPDX_ID = "SPDXRef-Package-Tessera"
DOCUMENT_SPDX_ID = "SPDXRef-DOCUMENT"
DOCUMENT_NAMESPACE_PREFIX = "https://github.com/thnak/Tessera/spdx"
DETERMINISTIC_CREATED = "1970-01-01T00:00:00Z"
TOOL_CREATOR = "Tool: tessera-gen-sbom"
SPDX_VERSION = "SPDX-2.3"
DATA_LICENSE = "CC0-1.0"

PUBLIC_DOMAIN_LICENSE = "Public domain"
PUBLIC_DOMAIN_REF = "LicenseRef-Public-Domain"
PUBLIC_DOMAIN_COMPONENT = "sqlite3"

# Canonical license name (as parsed from docs/specs/dependencies.md) -> SPDX license id.
# Non-SPDX licenses use an SPDX LicenseRef and are described in hasExtractedLicensingInfos.
SPDX_LICENSE_IDS: Dict[str, str] = {
    "MIT": "MIT",
    "MIT-0": "MIT-0",
    "BSD-2-Clause": "BSD-2-Clause",
    "BSD-3-Clause": "BSD-3-Clause",
    "Apache-2.0": "Apache-2.0",
    "FTL": "FTL",
    "Unicode": "Unicode-3.0",
    "zlib": "Zlib",
    "libpng": "Libpng",
    "IJG": "IJG",
    "ISC": "ISC",
    "curl": "curl",
    "BSL-1.0": "BSL-1.0",
    "OFL": "OFL-1.1",
    PUBLIC_DOMAIN_LICENSE: PUBLIC_DOMAIN_REF,
}

VERSION_RE = re.compile(r"(?<![A-Za-z0-9_.])v?(\d+\.\d+(?:\.\d+)*)(?![A-Za-z0-9_.])")
URL_RE = re.compile(r"https?://[^\s,;)\]]+")
PROJECT_VERSION_RE = re.compile(
    r"project\s*\(\s*Tessera\s+VERSION\s+([0-9]+\.[0-9]+(?:\.[0-9]+)*)",
    re.IGNORECASE,
)


class SbomError(Exception):
    """A verification or data failure that must fail the gate."""


def clean(text: str) -> str:
    return bundle.clean_text(text)


def parse_project_version(cmake_text: str) -> str:
    match = PROJECT_VERSION_RE.search(cmake_text)
    return match.group(1) if match else "0.0.0"


def parse_project_copyright(license_text: str) -> str:
    for line in license_text.splitlines():
        match = re.match(r"^\s*(Copyright\b.*)$", line)
        if match:
            return clean(match.group(1))
    return "NOASSERTION"


def version_from(*texts: Optional[str]) -> Optional[str]:
    for text in texts:
        if not text:
            continue
        match = VERSION_RE.search(text)
        if match:
            return match.group(1)
    return None


def upstream_url(text: Optional[str]) -> Optional[str]:
    if not text:
        return None
    match = URL_RE.search(text)
    return match.group(0).rstrip(".,;") if match else None


def spdx_license_expression(licenses: Sequence[str], component: str) -> str:
    identifiers: List[str] = []
    for license_name in licenses:
        identifier = SPDX_LICENSE_IDS.get(license_name)
        if identifier is None:
            raise SbomError(
                "cannot map license {!r} of {!r} to an SPDX license id; add it to "
                "SPDX_LICENSE_IDS".format(license_name, component)
            )
        identifiers.append(identifier)
    if not identifiers:
        raise SbomError("component {!r} has no license".format(component))
    return " OR ".join(identifiers)


def spdx_package_id(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9.]+", "-", name).strip("-.")
    if not cleaned:
        cleaned = "Component"
    return "SPDXRef-Package-" + cleaned


def build_extracted_infos(licenses_dir: Path) -> List[Dict[str, object]]:
    filename = bundle.LICENSE_FILES.get(PUBLIC_DOMAIN_LICENSE)
    if filename is None:
        raise SbomError(
            "no license text mapping for {!r}; add it to gen_license_bundle.LICENSE_FILES".format(
                PUBLIC_DOMAIN_LICENSE
            )
        )
    try:
        text = bundle.load_license_text(
            licenses_dir, PUBLIC_DOMAIN_LICENSE, [PUBLIC_DOMAIN_COMPONENT]
        )
    except bundle.BundleError as error:
        raise SbomError(str(error))
    source: Optional[str] = None
    body: List[str] = []
    in_body = False
    for line in text.splitlines():
        if not in_body:
            if line.startswith("Source:"):
                source = line.partition(":")[2].strip()
            if not line.strip():
                in_body = True
            continue
        body.append(line)
    extracted = "\n".join(body).strip("\n")
    if not extracted:
        raise SbomError("public-domain license text in {} is empty".format(filename))
    info: Dict[str, object] = {
        "licenseId": PUBLIC_DOMAIN_REF,
        "name": "Public domain (no SPDX license id)",
        "extractedText": extracted,
    }
    if source:
        info["seeAlsos"] = [source]
    return [info]


def build_document(
    dependencies_text: str,
    notices_text: str,
    licenses_dir: Path,
    project_version: str,
    project_copyright: str,
) -> Tuple[Dict[str, object], int, int]:
    try:
        components = bundle.production_components(dependencies_text)
    except bundle.BundleError as error:
        raise SbomError(str(error))
    entries = bundle.parse_notice_entries(notices_text)

    packages: List[Dict[str, object]] = []
    used_refs = set()
    for component in components:
        entry = bundle.notice_entry_for(component.name, entries)
        if entry is None or not entry.copyright:
            raise SbomError(
                "no copyright holder for production-linked component {!r}; add it to "
                "THIRD_PARTY_NOTICES.md".format(component.name)
            )
        expression = spdx_license_expression(component.licenses, component.name)
        if PUBLIC_DOMAIN_REF in expression:
            used_refs.add(PUBLIC_DOMAIN_REF)
        package: Dict[str, object] = {
            "name": component.name,
            "downloadLocation": upstream_url(entry.upstream) or "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": expression,
            "licenseDeclared": expression,
            "copyrightText": entry.copyright,
            "supplier": "NOASSERTION",
        }
        version = version_from(component.status, entry.upstream)
        if version:
            package["versionInfo"] = version
        packages.append(package)
    packages.sort(key=lambda item: (str(item["name"]).casefold(), str(item["name"])))

    used_ids = set()
    for package in packages:
        base = spdx_package_id(str(package["name"]))
        candidate = base
        suffix = 2
        while candidate.lower() in used_ids:
            candidate = "{}-{}".format(base, suffix)
            suffix += 1
        used_ids.add(candidate.lower())
        package["SPDXID"] = candidate

    extracted_infos = build_extracted_infos(licenses_dir) if used_refs else []

    fingerprint = json.dumps(
        {
            "project_version": project_version,
            "packages": [
                {
                    "name": package["name"],
                    "version": package.get("versionInfo", ""),
                    "license": package["licenseDeclared"],
                }
                for package in packages
            ],
        },
        sort_keys=True,
    )
    digest = hashlib.sha256(fingerprint.encode("utf-8")).hexdigest()[:16]
    document_namespace = "{}/tessera-{}-{}".format(
        DOCUMENT_NAMESPACE_PREFIX, project_version, digest
    )

    project_package: Dict[str, object] = {
        "name": PROJECT_NAME,
        "SPDXID": PROJECT_SPDX_ID,
        "versionInfo": project_version,
        "downloadLocation": PROJECT_DOWNLOAD_LOCATION,
        "filesAnalyzed": False,
        "licenseConcluded": "MIT",
        "licenseDeclared": "MIT",
        "copyrightText": project_copyright,
        "supplier": "NOASSERTION",
    }

    relationships: List[Dict[str, str]] = [
        {
            "spdxElementId": DOCUMENT_SPDX_ID,
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": PROJECT_SPDX_ID,
        }
    ]
    relationships.extend(
        {
            "spdxElementId": PROJECT_SPDX_ID,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": str(package["SPDXID"]),
        }
        for package in packages
    )
    relationships[1:] = sorted(
        relationships[1:], key=lambda item: str(item["relatedSpdxElement"])
    )

    document: Dict[str, object] = {
        "SPDXID": DOCUMENT_SPDX_ID,
        "spdxVersion": SPDX_VERSION,
        "dataLicense": DATA_LICENSE,
        "name": "{}-{}".format(PROJECT_NAME, project_version),
        "documentNamespace": document_namespace,
        "creationInfo": {
            "created": DETERMINISTIC_CREATED,
            "creators": [TOOL_CREATOR],
        },
        "comment": (
            "Deterministic SBOM for the shipped Tessera runtime: production-linked components "
            "only, no build-time/CI/design-time rows; creation time and tool version are fixed so "
            "repeated runs are byte-identical. filesAnalyzed is false: no per-file hashes."
        ),
        "packages": [project_package] + packages,
        "relationships": relationships,
    }
    if extracted_infos:
        document["hasExtractedLicensingInfos"] = extracted_infos

    license_count = len({str(package["licenseDeclared"]) for package in packages})
    return document, len(packages), license_count


def render(document: Dict[str, object]) -> str:
    return json.dumps(document, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


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
    mode.add_argument("--write", action="store_true", help="write sbom.spdx.json")
    mode.add_argument("--check", action="store_true", help="verify the SBOM is up to date")
    parser.add_argument("--dependencies", default=str(DEFAULT_DEPENDENCIES))
    parser.add_argument("--notices", default=str(DEFAULT_NOTICES))
    parser.add_argument("--licenses-dir", default=str(DEFAULT_LICENSES_DIR))
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("--license-file", default=str(DEFAULT_LICENSE_FILE))
    parser.add_argument("--cmake-lists", default=str(DEFAULT_CMAKELISTS))
    parser.add_argument("--json", action="store_true", dest="as_json", help="machine-readable summary")
    args = parser.parse_args(argv)

    dependencies_path = Path(args.dependencies)
    notices_path = Path(args.notices)
    licenses_dir = Path(args.licenses_dir)
    output_path = Path(args.output)
    license_path = Path(args.license_file)
    cmake_path = Path(args.cmake_lists)
    for path in (dependencies_path, notices_path, license_path, cmake_path):
        if not path.is_file():
            print("error: file not found: {}".format(path), file=sys.stderr)
            return 2
    if not licenses_dir.is_dir():
        print("error: license text directory not found: {}".format(licenses_dir), file=sys.stderr)
        return 2

    project_version = parse_project_version(cmake_path.read_text(encoding="utf-8"))
    project_copyright = parse_project_copyright(license_path.read_text(encoding="utf-8"))
    try:
        document, component_count, license_count = build_document(
            dependencies_path.read_text(encoding="utf-8"),
            notices_path.read_text(encoding="utf-8"),
            licenses_dir,
            project_version,
            project_copyright,
        )
    except SbomError as error:
        print("error: {}".format(error), file=sys.stderr)
        return 1

    text = render(document)
    size = len(text.encode("utf-8"))
    summary = {
        "ok": True,
        "gate": "sbom",
        "file": str(output_path),
        "spdx_version": SPDX_VERSION,
        "package_count": component_count + 1,
        "component_count": component_count,
        "license_expression_count": license_count,
        "extracted_license_count": len(document.get("hasExtractedLicensingInfos", [])),
        "size_bytes": size,
    }

    if args.write:
        output_path.write_text(text, encoding="utf-8", newline="")
        if args.as_json:
            print(json.dumps(summary, indent=2, sort_keys=True))
        else:
            print(
                "wrote {} ({}, {} components + project, {} license expression(s), {} bytes)".format(
                    output_path, SPDX_VERSION, component_count, license_count, size
                )
            )
        return 0

    if not output_path.is_file():
        print(
            "error: {} is missing; run `python -B scripts/gen_sbom.py --write`".format(
                output_path
            ),
            file=sys.stderr,
        )
        return 1
    actual = output_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    if actual != text:
        if args.as_json:
            summary["ok"] = False
            summary["error"] = "stale"
            print(json.dumps(summary, indent=2, sort_keys=True))
        print(
            "error: {} is stale ({}); run `python -B scripts/gen_sbom.py --write`".format(
                output_path, describe_drift(text, actual)
            ),
            file=sys.stderr,
        )
        return 1
    if args.as_json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print(
            "{} is up to date ({}, {} components + project, {} license expression(s), "
            "{} bytes)".format(output_path, SPDX_VERSION, component_count, license_count, size)
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
