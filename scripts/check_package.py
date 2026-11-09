#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate a Tessera distributable archive.

Checks a CPack ``.tar.gz``/``.tgz`` (or ``.zip``) produced with
``-DTESSERA_PACKAGE_EXECUTABLES=...`` before it is published:

* one ``tessera-<version>-<os>-<arch>/`` top-level directory, with the executables listed in
  ``PACKAGE-INFO.txt`` and the compliance files next to them (``LICENSE``,
  ``THIRD_PARTY_NOTICES.md``, ``THIRD_PARTY_LICENSES.txt``, ``sbom.spdx.json``);
* ``PACKAGE-INFO.txt`` records version/commit/platform/static runtime and leaks no absolute build
  paths or timestamps; the static C/C++ runtime must be ON (ADR-012);
* ``sbom.spdx.json`` parses as an SPDX-2.3 JSON document with the expected shape;
* ``THIRD_PARTY_LICENSES.txt`` contains a text section and a component row for every
  production-linked component (reusing the dependency/license helpers);
* no unexpected files or file types (object/library/debug/build artifacts).

Stdlib only, deterministic sorted output. Exit codes: 0 pass, 1 validation failure,
2 usage/unreadable input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
DEFAULT_DEPENDENCIES = REPO_ROOT / "docs" / "specs" / "dependencies.md"

if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import gen_license_bundle as bundle  # noqa: E402  (sibling script, stdlib only)

REQUIRED_FILES = (
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "THIRD_PARTY_LICENSES.txt",
    "sbom.spdx.json",
    "PACKAGE-INFO.txt",
    "RELEASE-NOTES.txt",
    "SHA256SUMS",
)
INFO_REQUIRED_KEYS = ("Version", "Commit", "Platform", "StaticRuntime", "Executables")
VERIFY_COMMANDS = ("sha256sum -c SHA256SUMS", "certutil", "Get-FileHash")

TOP_DIR_RE = re.compile(r"^tessera-[0-9A-Za-z][0-9A-Za-z._-]*$")
SHA_LINE_RE = re.compile(r"^([0-9a-f]{64})  (.+)$")
ABSOLUTE_BUILD_PATH_RE = re.compile(
    r"(?i)(?:[A-Za-z]:[\\/]|\\\\|/(?:home|users|tmp|var|build|builds|mnt|opt|workspace|root)/)"
)
TIMESTAMP_RE = re.compile(
    r"\b\d{4}-\d{2}-\d{2}(?:[T ]\d{2}:\d{2}(?::\d{2})?(?:Z|[+-]\d{2}:?\d{2})?)?\b"
)
UNEXPECTED_SUFFIXES = (
    ".o",
    ".obj",
    ".a",
    ".lib",
    ".exp",
    ".pdb",
    ".ilk",
    ".dSYM",
    ".cmake",
    ".ninja",
    ".log",
)
UNEXPECTED_PATH_MARKERS = ("CMakeFiles/", "_deps/", "/build/", ".dir/")

SPDX_REQUIRED_KEYS = ("spdxVersion", "dataLicense", "SPDXID", "name", "documentNamespace")
SPDX_PACKAGE_KEYS = ("name", "SPDXID", "licenseConcluded", "licenseDeclared", "filesAnalyzed")


class PackageError(Exception):
    """A validation failure (exit code 1)."""


class Archive:
    def __init__(self, kind: str, files: Dict[str, bytes], modes: Dict[str, int]) -> None:
        self.kind = kind
        self.files = files
        self.modes = modes


def read_archive(path: Path) -> Archive:
    if zipfile.is_zipfile(path):
        files: Dict[str, bytes] = {}
        modes: Dict[str, int] = {}
        try:
            with zipfile.ZipFile(path) as archive:
                for info in archive.infolist():
                    name = info.filename
                    if name.endswith("/"):
                        continue
                    validate_member_name(name)
                    files[name] = archive.read(info)
                    modes[name] = (info.external_attr >> 16) & 0o777
        except (zipfile.BadZipFile, OSError, RuntimeError) as error:
            raise PackageError("not a readable zip archive: {}".format(error))
        return Archive("zip", files, modes)

    try:
        archive = tarfile.open(path, "r:*")
    except (tarfile.TarError, OSError):
        raise PackageError(
            "not a package archive: {} is neither a zip nor a tar archive".format(path.name)
        )
    files = {}
    modes = {}
    with archive:
        for member in archive.getmembers():
            if member.isdir():
                continue
            if not member.isfile():
                raise PackageError(
                    "unsupported archive entry type for '{}' (only regular files are allowed)".format(
                        member.name
                    )
                )
            validate_member_name(member.name)
            extracted = archive.extractfile(member)
            if extracted is None:
                raise PackageError("could not read archive entry '{}'".format(member.name))
            files[member.name] = extracted.read()
            modes[member.name] = member.mode & 0o777
    return Archive("tar", files, modes)


def validate_member_name(name: str) -> None:
    if name.startswith("/") or re.match(r"^[A-Za-z]:[\\/]", name):
        raise PackageError("archive contains an absolute path: '{}'".format(name))
    parts = name.replace("\\", "/").split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise PackageError("archive contains an unsafe path: '{}'".format(name))


def parse_info(text: str) -> Tuple[Dict[str, str], List[str]]:
    errors: List[str] = []
    info: Dict[str, str] = {}
    for index, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped == "Tessera package":
            continue
        if ":" not in stripped:
            errors.append("PACKAGE-INFO.txt:{}: expected 'Key: value'".format(index))
            continue
        key, _, value = stripped.partition(":")
        info[key.strip()] = value.strip()
        if ABSOLUTE_BUILD_PATH_RE.search(value):
            errors.append(
                "PACKAGE-INFO.txt:{}: absolute build path leaked: '{}'".format(index, value)
            )
        if TIMESTAMP_RE.search(value):
            errors.append(
                "PACKAGE-INFO.txt:{}: build timestamp leaked: '{}'".format(index, value)
            )
    for key in INFO_REQUIRED_KEYS:
        if not info.get(key):
            errors.append("PACKAGE-INFO.txt: missing required '{}:' line".format(key))
    if info.get("StaticRuntime") and info["StaticRuntime"].upper() != "ON":
        errors.append(
            "PACKAGE-INFO.txt: StaticRuntime is '{}'; ADR-012 requires a static C/C++ runtime"
            .format(info["StaticRuntime"])
        )
    return info, errors


def validate_release_notes(
    text: str, package_info: Dict[str, str], errors: List[str]
) -> None:
    notes: Dict[str, str] = {}
    for line in text.splitlines():
        if ":" not in line or line.lstrip().startswith("-"):
            continue
        key, _, value = line.partition(":")
        notes[key.strip()] = value.strip()
    for key in INFO_REQUIRED_KEYS:
        if not notes.get(key):
            errors.append("RELEASE-NOTES.txt: missing required '{}:' line".format(key))
        elif package_info.get(key) and notes[key] != package_info[key]:
            errors.append(
                "RELEASE-NOTES.txt: '{}' does not match PACKAGE-INFO.txt".format(key)
            )
    for command in VERIFY_COMMANDS:
        if command not in text:
            errors.append(
                "RELEASE-NOTES.txt: missing verification command {!r}".format(command)
            )
    for line in text.splitlines():
        if ABSOLUTE_BUILD_PATH_RE.search(line):
            errors.append(
                "RELEASE-NOTES.txt: absolute build path leaked: '{}'".format(line.strip())
            )
        if TIMESTAMP_RE.search(line):
            errors.append(
                "RELEASE-NOTES.txt: build timestamp leaked: '{}'".format(line.strip())
            )


def validate_sha256sums(
    files: Dict[str, bytes], inner: Dict[str, str], errors: List[str]
) -> int:
    data = files[inner["SHA256SUMS"]]
    if b"\r" in data:
        errors.append("SHA256SUMS: must use LF line endings")
    text = data.decode("utf-8", errors="replace")
    entries: List[Tuple[str, str]] = []
    for index, line in enumerate(text.splitlines(), start=1):
        if not line:
            continue
        match = SHA_LINE_RE.match(line)
        if not match:
            errors.append(
                "SHA256SUMS:{}: expected '<sha256>  <file>' with two spaces".format(index)
            )
            continue
        digest, name = match.groups()
        if name == "SHA256SUMS":
            errors.append("SHA256SUMS: must not list itself")
            continue
        if name != name.rsplit("/", 1)[-1] or name in ("", ".", ".."):
            errors.append("SHA256SUMS:{}: unsafe or nested path '{}'".format(index, name))
            continue
        entries.append((name, digest))
    names = [name for name, _ in entries]
    if names != sorted(names):
        errors.append("SHA256SUMS: entries are not sorted by file name")
    if len(set(names)) != len(names):
        errors.append("SHA256SUMS: duplicate file entries")
    expected = set(inner) - {"SHA256SUMS"}
    for name in sorted(expected - set(names)):
        errors.append("SHA256SUMS: missing entry for '{}'".format(name))
    for name in sorted(set(names) - expected):
        errors.append("SHA256SUMS: entry for unknown file '{}'".format(name))
    for name, digest in entries:
        if name not in inner:
            continue
        actual = hashlib.sha256(files[inner[name]]).hexdigest()
        if actual != digest:
            errors.append("SHA256SUMS: hash mismatch for '{}'".format(name))
    return len(entries)


def executable_names(info: Dict[str, str]) -> List[str]:
    raw = info.get("Executables", "")
    return [name for name in re.split(r"[,; ]+", raw) if name]


def validate_sbom(data: bytes, errors: List[str]) -> int:
    try:
        document = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        errors.append("sbom.spdx.json: not valid JSON: {}".format(error))
        return 0
    if not isinstance(document, dict):
        errors.append("sbom.spdx.json: top-level JSON value is not an object")
        return 0
    for key in SPDX_REQUIRED_KEYS:
        if not document.get(key):
            errors.append("sbom.spdx.json: missing required SPDX field '{}'".format(key))
    if document.get("spdxVersion") != "SPDX-2.3":
        errors.append(
            "sbom.spdx.json: spdxVersion is {!r}, expected 'SPDX-2.3'".format(
                document.get("spdxVersion")
            )
        )
    if document.get("dataLicense") != "CC0-1.0":
        errors.append(
            "sbom.spdx.json: dataLicense is {!r}, expected 'CC0-1.0'".format(
                document.get("dataLicense")
            )
        )
    packages = document.get("packages")
    if not isinstance(packages, list) or not packages:
        errors.append("sbom.spdx.json: 'packages' must be a non-empty array")
        return 0
    for index, package in enumerate(packages):
        if not isinstance(package, dict):
            errors.append("sbom.spdx.json: packages[{}] is not an object".format(index))
            continue
        for key in SPDX_PACKAGE_KEYS:
            if key not in package:
                errors.append(
                    "sbom.spdx.json: packages[{}] ({!r}) is missing '{}'".format(
                        index, package.get("name", "?"), key
                    )
                )
        if package.get("filesAnalyzed") is not False:
            errors.append(
                "sbom.spdx.json: packages[{}] must set filesAnalyzed: false".format(index)
            )
    for info_index, extracted in enumerate(document.get("hasExtractedLicensingInfos", [])):
        if not isinstance(extracted, dict) or not extracted.get("licenseId"):
            errors.append(
                "sbom.spdx.json: hasExtractedLicensingInfos[{}] has no licenseId".format(
                    info_index
                )
            )
        elif not extracted.get("extractedText"):
            errors.append(
                "sbom.spdx.json: hasExtractedLicensingInfos[{}] has no extractedText".format(
                    info_index
                )
            )
    return len(packages)


def validate_bundle_text(
    bundle_text: str, dependencies_text: str, errors: List[str]
) -> None:
    try:
        components = bundle.production_components(dependencies_text)
    except bundle.BundleError as error:
        raise PackageError(str(error))
    sections = set(re.findall(r"^License: (.+)$", bundle_text, re.MULTILINE))
    for component in components:
        if component.name not in bundle_text:
            errors.append(
                "THIRD_PARTY_LICENSES.txt: component {!r} is missing".format(component.name)
            )
        for license_name in component.licenses:
            if license_name not in sections:
                errors.append(
                    "THIRD_PARTY_LICENSES.txt: missing license text section for {!r} "
                    "(used by {!r})".format(license_name, component.name)
                )


def check_package(archive_path: Path, dependencies_path: Path) -> Tuple[Dict[str, object], List[str]]:
    if not archive_path.is_file():
        raise PackageError("archive not found: {}".format(archive_path))
    if not dependencies_path.is_file():
        raise PackageError("dependency map not found: {}".format(dependencies_path))

    archive = read_archive(archive_path)
    files = archive.files
    errors: List[str] = []

    top_level = sorted({name.split("/")[0] for name in files if "/" in name})
    root_files = sorted(name for name in files if "/" not in name)
    if root_files:
        errors.append(
            "archive: files outside the top-level package directory: {}".format(
                ", ".join(root_files[:5])
            )
        )
    if len(top_level) != 1:
        errors.append(
            "archive: expected exactly one top-level 'tessera-<version>-<os>-<arch>/' directory, "
            "found {}".format(len(top_level))
        )
        return {"files": sorted(files)}, errors
    top = top_level[0]
    if not TOP_DIR_RE.match(top):
        errors.append(
            "archive: top-level directory {!r} is not named tessera-<version>-<os>-<arch>".format(
                top
            )
        )

    inner: Dict[str, str] = {}
    for name in files:
        prefix = top + "/"
        if name.startswith(prefix):
            relative = name[len(prefix) :]
            inner[relative] = name
    for required in REQUIRED_FILES:
        if required not in inner:
            errors.append("archive: missing required file '{}'".format(required))

    package_info: Dict[str, str] = {}
    if "PACKAGE-INFO.txt" in inner:
        info_text = files[inner["PACKAGE-INFO.txt"]].decode("utf-8", errors="replace")
        package_info, info_errors = parse_info(info_text)
        errors.extend(info_errors)
        version = package_info.get("Version")
        platform = package_info.get("Platform")
        if version and platform:
            expected = "tessera-{}-{}".format(version, platform)
            if top != expected:
                errors.append(
                    "archive: top-level directory '{}' does not match version/platform "
                    "'{}'".format(top, expected)
                )

    expected_files: Set[str] = set(REQUIRED_FILES)
    for executable in executable_names(package_info):
        candidates = [executable, executable + ".exe"]
        found = [candidate for candidate in candidates if candidate in inner]
        if not found:
            errors.append(
                "archive: executable '{}' listed in PACKAGE-INFO.txt is missing".format(executable)
            )
        else:
            expected_files.add(found[0])
    allowed_executables = set(executable_names(package_info))
    for relative, name in sorted(inner.items()):
        base = relative.rsplit("/", 1)[-1]
        if "/" in relative:
            errors.append(
                "archive: unexpected nested path '{}' (package files must be flat)".format(
                    relative
                )
            )
            continue
        if base in expected_files:
            continue
        if base in {candidate + ".exe" for candidate in allowed_executables}:
            continue
        if base in allowed_executables:
            continue
        errors.append("archive: unexpected file '{}'".format(relative))
        continue
    for name in files:
        lowered = name.lower()
        if lowered.endswith(UNEXPECTED_SUFFIXES) or any(
            marker in name for marker in UNEXPECTED_PATH_MARKERS
        ):
            errors.append("archive: unexpected build artifact '{}'".format(name))

    if "THIRD_PARTY_LICENSES.txt" in inner:
        bundle_text = files[inner["THIRD_PARTY_LICENSES.txt"]].decode(
            "utf-8", errors="replace"
        )
        try:
            validate_bundle_text(
                bundle_text,
                dependencies_path.read_text(encoding="utf-8"),
                errors,
            )
        except PackageError as error:
            errors.append("THIRD_PARTY_LICENSES.txt: {}".format(error))
    if "LICENSE" in inner and not files[inner["LICENSE"]].strip():
        errors.append("archive: LICENSE is empty")
    if "RELEASE-NOTES.txt" in inner:
        release_notes_data = files[inner["RELEASE-NOTES.txt"]]
        if b"\r" in release_notes_data:
            errors.append("RELEASE-NOTES.txt: must use LF line endings")
        validate_release_notes(
            release_notes_data.decode("utf-8", errors="replace"), package_info, errors
        )
    checksum_entries = 0
    if "SHA256SUMS" in inner:
        checksum_entries = validate_sha256sums(files, inner, errors)

    package_count = 0
    if "sbom.spdx.json" in inner:
        package_count = validate_sbom(files[inner["sbom.spdx.json"]], errors)

    summary: Dict[str, object] = {
        "files": sorted(inner),
        "archive_type": archive.kind,
        "package": {
            "version": package_info.get("Version", ""),
            "platform": package_info.get("Platform", ""),
            "commit": package_info.get("Commit", ""),
            "static_runtime": package_info.get("StaticRuntime", ""),
            "executables": executable_names(package_info),
        },
        "sbom_packages": package_count,
        "checksum_entries": checksum_entries,
    }
    errors.sort()
    return summary, errors


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--archive", required=True, help=".tar.gz/.tgz/.zip package to validate")
    parser.add_argument("--dependencies", default=str(DEFAULT_DEPENDENCIES))
    parser.add_argument("--json", action="store_true", dest="as_json", help="machine-readable report")
    args = parser.parse_args(argv)

    archive_path = Path(args.archive)
    dependencies_path = Path(args.dependencies)
    try:
        summary, errors = check_package(archive_path, dependencies_path)
    except PackageError as error:
        message = str(error)
        setup_failure = message.startswith("archive not found") or message.startswith(
            "dependency map not found"
        )
        payload = {
            "ok": False,
            "gate": "package",
            "archive": str(archive_path),
            "errors": [message],
        }
        if args.as_json:
            print(json.dumps(payload, indent=2, sort_keys=True))
        print("error: {}".format(message), file=sys.stderr)
        return 2 if setup_failure else 1

    ok = not errors
    if args.as_json:
        payload = {
            "ok": ok,
            "gate": "package",
            "archive": str(archive_path),
            "errors": errors,
        }
        payload.update(summary)
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        for error in errors:
            print("ERROR {}".format(error))
        print(
            "package gate: {} error(s), {} file(s), {} SBOM package(s) ({})".format(
                len(errors),
                len(summary.get("files", [])),
                summary.get("sbom_packages", 0),
                summary.get("archive_type", "?"),
            )
        )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
