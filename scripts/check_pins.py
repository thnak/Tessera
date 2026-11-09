#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Supply-chain reproducibility gate: every fetched dependency must have an exact pin.

Scans the project CMake sources (root ``CMakeLists.txt`` and ``*.cmake``/``CMakeLists.txt`` under
``cmake/``, ``third_party/`` and ``src/``) for ``FetchContent_Declare``, ``ExternalProject_Add``,
``FetchContent_MakeAvailable``, ``file(DOWNLOAD ...)`` and vendored ``SOURCE_DIR`` declarations.
Each fetch must use either a full 40-hex commit ``GIT_TAG`` (with ``GIT_REPOSITORY``) or an
``URL_HASH SHA256=<64 hex>``. Branches, release tags, short SHAs, ``HEAD`` and hashes other than
SHA-256 are errors, even when a fetch is option-gated or ``EXCLUDE_FROM_ALL``. ``FetchContent_
MakeAvailable`` names must resolve to a declaration in the scanned sources.

Every declared component must also have a row in ``docs/specs/dependencies.md`` (reusing the
dependency parser), so a new fetch cannot bypass the license/notice/SBOM pipeline.

Stdlib only, deterministic sorted output; exit 1 on any error, 0 with warnings only, 2 usage.
Run from any directory: ``python -B scripts/check_pins.py [--json] [--repo-root PATH]``.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
DEFAULT_DEPENDENCIES = REPO_ROOT / "docs" / "specs" / "dependencies.md"

if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import check_dependencies as deps  # noqa: E402  (sibling script, stdlib only)

# Documented exceptions: (repo-relative CMake file, component name) -> reason. Exceptions downgrade
# that declaration's pin errors to a warning; a stale entry (no matching error) is reported too.
PIN_ALLOW_LIST: Dict[Tuple[str, str], str] = {
    # ("third_party/CMakeLists.txt", "example"): "owner-approved unpinned fetch, reason here",
}

SCAN_DIRS = ("cmake", "third_party", "src")
DECLARE_RE = re.compile(r"\b(FetchContent_Declare|ExternalProject_Add)\s*\(")
DOWNLOAD_RE = re.compile(r"\bfile\s*\(\s*DOWNLOAD\b")
MAKE_AVAILABLE_RE = re.compile(r"\bFetchContent_MakeAvailable\s*\(\s*([^)\s]+)")
GIT_SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")
SHA256_RE = re.compile(r"^SHA256=([0-9a-fA-F]{64})$")
KEY_VALUE_RE = re.compile(r'(?P<key>[A-Za-z_][A-Za-z0-9_]*)\s+(?P<value>"[^"]*"|[^\s)]+)')
SET_OPEN_RE = re.compile(r"\bset\s*\(")
VARIABLE_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


@dataclass(frozen=True)
class Pin:
    component: str
    file: str
    line: int
    kind: str
    value: str
    dependency_row: str


@dataclass(frozen=True, order=True)
class Issue:
    file: str
    line: int
    severity: str
    message: str


class PinError(Exception):
    """Unusable input (exit code 2)."""


def strip_comment(line: str) -> str:
    in_quote = False
    result: List[str] = []
    for character in line:
        if character == '"':
            in_quote = not in_quote
            result.append(character)
            continue
        if character == "#" and not in_quote:
            break
        result.append(character)
    return "".join(result)


def cmake_files(repo_root: Path) -> List[Path]:
    files: List[Path] = []
    root = repo_root / "CMakeLists.txt"
    if root.is_file():
        files.append(root)
    for name in SCAN_DIRS:
        directory = repo_root / name
        if not directory.is_dir():
            continue
        files.extend(sorted(directory.rglob("CMakeLists.txt")))
        files.extend(sorted(directory.rglob("*.cmake")))
    unique: List[Path] = []
    seen: Set[Path] = set()
    for path in sorted(files, key=lambda item: item.relative_to(repo_root).as_posix()):
        resolved = path.resolve()
        if resolved not in seen:
            seen.add(resolved)
            unique.append(path)
    return unique


class PinChecker:
    def __init__(self, repo_root: Path, dependencies_text: str) -> None:
        self.repo_root = repo_root
        self.rows = deps.parse_dependency_rows(dependencies_text)
        self.issues: List[Issue] = []
        self.pins: List[Pin] = []
        self.declarations: Set[str] = set()
        self.make_available: List[Tuple[str, str, int]] = []
        self.variables: Dict[str, List[Tuple[str, int, str]]] = {}
        self.used_allow_list: Set[Tuple[str, str]] = set()

    def rel(self, path: Path) -> str:
        return path.relative_to(self.repo_root).as_posix()

    def add(self, file: str, line: int, severity: str, message: str) -> None:
        self.issues.append(Issue(file, line, severity, message))

    def collect_variables(self, path: Path, lines: Sequence[str]) -> None:
        rel = self.rel(path)
        index = 0
        while index < len(lines):
            text = strip_comment(lines[index])
            open_match = SET_OPEN_RE.search(text)
            if not open_match:
                index += 1
                continue
            end = self.block_end(lines, index, open_match.end() - 1)
            block = " ".join(
                strip_comment(lines[position]) for position in range(index, end + 1)
            )
            content = block[open_match.end() :].rstrip()
            if content.endswith(")"):
                content = content[:-1]
            assignment = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s+(.*)", content, re.DOTALL)
            if assignment:
                value = assignment.group(2).strip()
                if value.startswith('"') and value.endswith('"'):
                    value = value[1:-1]
                self.variables.setdefault(assignment.group(1), []).append(
                    (rel, index + 1, value)
                )
            index = end + 1

    def expand(self, text: str, rel: str, line: int) -> Tuple[str, bool]:
        resolved = text
        unresolved = False
        source_path = self.repo_root / rel
        builtins = {
            "CMAKE_CURRENT_SOURCE_DIR": str(source_path.parent),
            "CMAKE_CURRENT_LIST_DIR": str(source_path.parent),
            "CMAKE_SOURCE_DIR": str(self.repo_root),
            "PROJECT_SOURCE_DIR": str(self.repo_root),
        }
        for name, value in builtins.items():
            resolved = resolved.replace("${" + name + "}", value)
        for _ in range(6):
            matches = VARIABLE_RE.findall(resolved)
            if not matches:
                break
            for name in matches:
                value = self.variable_value(name, rel, line)
                if value is None:
                    unresolved = True
                    continue
                resolved = resolved.replace("${" + name + "}", value)
            if unresolved:
                break
        return resolved, unresolved

    def variable_value(self, name: str, rel: str, line: int) -> Optional[str]:
        candidates = self.variables.get(name)
        if not candidates:
            return None
        same_file = [entry for entry in candidates if entry[0] == rel and entry[1] < line]
        if same_file:
            return same_file[-1][2].strip('"')
        return candidates[0][2].strip('"')

    def find_dependency_row(self, component: str) -> Optional[str]:
        component_tokens = set(deps.name_tokens(component))
        if not component_tokens:
            return None
        for row in self.rows:
            row_tokens = set(deps.name_tokens(row.name))
            if not row_tokens:
                continue
            if component_tokens == row_tokens or (component_tokens & row_tokens):
                return row.name
        return None

    def block_end(self, lines: Sequence[str], start: int, open_index: int) -> int:
        depth = 0
        for index in range(start, len(lines)):
            text = strip_comment(lines[index])
            begin = open_index if index == start else 0
            for position in range(begin, len(text)):
                character = text[position]
                if character == "(":
                    depth += 1
                elif character == ")":
                    depth -= 1
                    if depth == 0:
                        return index
        return len(lines) - 1

    def scan_file(self, path: Path) -> None:
        rel = self.rel(path)
        lines = path.read_text(encoding="utf-8").splitlines()
        self.collect_variables(path, lines)
        index = 0
        while index < len(lines):
            text = strip_comment(lines[index])
            declare = DECLARE_RE.search(text)
            download = DOWNLOAD_RE.search(text)
            if declare:
                end = self.block_end(lines, index, declare.end() - 1)
                block = "\n".join(
                    strip_comment(lines[position]) for position in range(index, end + 1)
                )
                content = block[declare.end() :]
                self.check_declare(rel, index + 1, content)
                index = end + 1
                continue
            if download:
                end = self.block_end(lines, index, download.end() - 1)
                block = "\n".join(
                    strip_comment(lines[position]) for position in range(index, end + 1)
                )
                self.check_download(rel, index + 1, block[download.end() :])
                index = end + 1
                continue
            for match in MAKE_AVAILABLE_RE.finditer(text):
                self.make_available.append(
                    (match.group(1).strip().lower(), rel, index + 1)
                )
            index += 1

    def check_declare(self, rel: str, line: int, content: str) -> None:
        expanded, unresolved = self.expand(content, rel, line)
        name_match = re.match(r"\s*([A-Za-z0-9_.+-]+)", expanded)
        if not name_match:
            self.add(rel, line, "ERROR", "fetch declaration has no component name")
            return
        component = name_match.group(1)
        self.declarations.add(component.lower())
        fields = self.parse_fields(expanded[name_match.end() :])
        if unresolved:
            self.add(
                rel,
                line,
                "ERROR",
                "{}: pin expression could not be resolved from the scanned CMake variables"
                .format(component),
            )
            return
        issues_before = len(self.issues)
        row = self.find_dependency_row(component)
        if fields.get("GIT_REPOSITORY"):
            tag = fields.get("GIT_TAG", "")
            if not tag:
                self.add(rel, line, "ERROR", "{}: git fetch has no GIT_TAG".format(component))
            elif GIT_SHA_RE.match(tag):
                self.record(component, rel, line, "git-commit", tag, row)
            else:
                self.add(
                    rel,
                    line,
                    "ERROR",
                    "{}: GIT_TAG '{}' is not a full 40-hex commit SHA; branches, release tags, "
                    "HEAD and short SHAs are not reproducible".format(component, tag),
                )
        elif fields.get("URL"):
            url_hash = fields.get("URL_HASH", "")
            if not url_hash:
                self.add(
                    rel,
                    line,
                    "ERROR",
                    "{}: URL fetch has no URL_HASH; add URL_HASH SHA256=<64 hex>".format(
                        component
                    ),
                )
            elif not SHA256_RE.match(url_hash):
                self.add(
                    rel,
                    line,
                    "ERROR",
                    "{}: URL_HASH '{}' is not SHA256=<64 hex>".format(component, url_hash),
                )
            else:
                self.record(component, rel, line, "url-sha256", url_hash, row)
        elif fields.get("SOURCE_DIR"):
            source_dir = fields["SOURCE_DIR"]
            candidate = Path(source_dir)
            if not candidate.is_absolute():
                candidate = (self.repo_root / source_dir).resolve()
            if not candidate.is_dir():
                self.add(
                    rel,
                    line,
                    "WARN",
                    "{}: vendored SOURCE_DIR does not exist: {}".format(component, source_dir),
                )
            self.record(component, rel, line, "vendored-source", source_dir, row)
        else:
            self.add(
                rel,
                line,
                "ERROR",
                "{}: fetch has no URL, GIT_REPOSITORY or SOURCE_DIR pin".format(component),
            )
        if row is None:
            self.add(
                rel,
                line,
                "ERROR",
                "{}: fetched component has no row in docs/specs/dependencies.md".format(component),
            )
        self.apply_allow_list(rel, component, line, issues_before)

    def check_download(self, rel: str, line: int, content: str) -> None:
        expanded, unresolved = self.expand(content, rel, line)
        fields = self.parse_fields(expanded)
        url = fields.get("URL", "")
        component = Path(url).name if url else "file-download"
        expected = fields.get("EXPECTED_HASH", "")
        if unresolved or not url:
            self.add(
                rel,
                line,
                "ERROR",
                "file(DOWNLOAD): URL could not be resolved from the scanned CMake variables",
            )
            return
        if not expected:
            self.add(
                rel,
                line,
                "ERROR",
                "{}: file(DOWNLOAD) has no EXPECTED_HASH SHA256=<64 hex>".format(component),
            )
        elif not SHA256_RE.match(expected):
            self.add(
                rel,
                line,
                "ERROR",
                "{}: EXPECTED_HASH '{}' is not SHA256=<64 hex>".format(component, expected),
            )
        else:
            self.pins.append(Pin(component, rel, line, "download-sha256", expected, ""))

    def parse_fields(self, content: str) -> Dict[str, str]:
        fields: Dict[str, str] = {}
        for match in KEY_VALUE_RE.finditer(content):
            key = match.group("key")
            if key not in {
                "URL",
                "URL_HASH",
                "GIT_REPOSITORY",
                "GIT_TAG",
                "SOURCE_DIR",
                "EXPECTED_HASH",
                "DOWNLOAD_EXTRACT_TIMESTAMP",
                "EXCLUDE_FROM_ALL",
                "GIT_SHALLOW",
                "EXTERNALPROJECT",
            }:
                continue
            value = match.group("value")
            if value.startswith('"') and value.endswith('"'):
                value = value[1:-1]
            fields[key] = value
        return fields

    def record(
        self,
        component: str,
        rel: str,
        line: int,
        kind: str,
        value: str,
        row: Optional[str],
    ) -> None:
        self.pins.append(Pin(component, rel, line, kind, value, row or ""))

    def apply_allow_list(self, rel: str, component: str, line: int, issues_before: int) -> None:
        new_issues = self.issues[issues_before:]
        key = (rel, component)
        if key not in PIN_ALLOW_LIST or not new_issues:
            return
        self.used_allow_list.add(key)
        del self.issues[issues_before:]
        self.add(
            rel,
            line,
            "WARN",
            "{}: pin errors allow-listed: {}".format(component, PIN_ALLOW_LIST[key]),
        )

    def run(self) -> Tuple[List[Pin], List[Issue]]:
        files = cmake_files(self.repo_root)
        if not files:
            raise PinError("no CMake files found under {}".format(self.repo_root))
        for path in files:
            self.scan_file(path)
        for name, rel, line in self.make_available:
            if name not in self.declarations:
                self.add(
                    rel,
                    line,
                    "ERROR",
                    "FetchContent_MakeAvailable({}) has no FetchContent_Declare in the scanned "
                    "CMake sources; add a pinned declaration".format(name),
                )
        for key in sorted(PIN_ALLOW_LIST):
            if key not in self.used_allow_list:
                self.add(
                    key[0],
                    1,
                    "WARN",
                    "stale PIN_ALLOW_LIST entry for '{}': remove it or restore the fetch".format(
                        key[1]
                    ),
                )
        pins = sorted(
            self.pins, key=lambda pin: (pin.file, pin.line, pin.component, pin.kind, pin.value)
        )
        issues = sorted(
            self.issues,
            key=lambda issue: (issue.file, issue.line, issue.severity, issue.message),
        )
        return pins, issues


def format_pin(pin: Pin) -> str:
    row = " row={}".format(pin.dependency_row) if pin.dependency_row else ""
    return "PIN {}:{} {} kind={} value={}{}".format(
        pin.file, pin.line, pin.component, pin.kind, pin.value, row
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument("--dependencies", default=str(DEFAULT_DEPENDENCIES))
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    repo_root = Path(args.repo_root).resolve()
    dependencies_path = Path(args.dependencies)
    if not repo_root.is_dir():
        print("error: repository root not found: {}".format(repo_root), file=sys.stderr)
        return 2
    if not dependencies_path.is_file():
        print("error: dependency map not found: {}".format(dependencies_path), file=sys.stderr)
        return 2

    checker = PinChecker(repo_root, dependencies_path.read_text(encoding="utf-8"))
    try:
        pins, issues = checker.run()
    except PinError as error:
        print("error: {}".format(error), file=sys.stderr)
        return 2
    errors = [issue for issue in issues if issue.severity == "ERROR"]
    warnings = [issue for issue in issues if issue.severity == "WARN"]

    if args.as_json:
        payload = {
            "ok": not errors,
            "gate": "pins",
            "pins": [pin.__dict__ for pin in pins],
            "errors": [issue.__dict__ for issue in errors],
            "warnings": [issue.__dict__ for issue in warnings],
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        for pin in pins:
            print(format_pin(pin))
        for issue in issues:
            print("{} {}:{} {}".format(issue.severity, issue.file, issue.line, issue.message))
        print(
            "pins gate: {} fetch(es), {} pinned, {} error(s), {} warning(s)".format(
                len(pins), len(pins), len(errors), len(warnings)
            )
        )
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
