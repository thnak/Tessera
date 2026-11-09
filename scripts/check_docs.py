#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Documentation integrity gate (AGENTS.md documentation rules).

Checks the repository markdown tree without writing anything:

* relative links and image targets resolve to existing files (external URLs are skipped, links
  that escape the repository are warnings);
* ``#fragment`` targets resolve to a GitHub-style heading slug in the target file;
* code fences balance, including nested fences (CommonMark rule: only a fence of the same
  character and at least the same length closes an open fence);
* every ``docs/**/*.md`` is reachable from ``docs/README.md`` (directly or through a folder
  ``README.md`` index) and every ``decisions/ADR-*.md`` is listed in ``decisions/README.md``;
* every ADR starts with an ``# `` heading and has a ``- Status:`` line;
* markdown files stay within the 400-line target / 600-line hard limit.

Output is deterministic and sorted: ``ERROR file:line message`` / ``WARN file:line message``.
Exit codes: 0 = no errors (warnings are fine), 1 = at least one error, 2 = usage error.

Run from any directory: ``python -B scripts/check_docs.py [--json] [--repo-root PATH]``.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple
from urllib.parse import unquote

REPO_ROOT = Path(__file__).resolve().parents[1]

# Root markdown files that are part of the documentation surface.
ROOT_MARKDOWN = (
    "README.md",
    "AGENTS.md",
    "CLAUDE.md",
    "CONTRIBUTING.md",
    "THIRD_PARTY_NOTICES.md",
)
EXTRA_MARKDOWN = ("licenses/README.md",)
# Root files grandfathered above the hard limit: report, never fail.
SIZE_EXEMPT_ROOT = ("AGENTS.md", "CLAUDE.md")

HARD_LIMIT = 600
SOFT_LIMIT = 400

# Intentionally unindexed docs. Keep this empty unless the document really must stay out of the
# docs map; the reason is printed as a warning so the exception is visible. A stale entry (the doc
# is now reachable) is also reported so the exemption is removed.
INDEX_ALLOW_LIST: Dict[str, str] = {
    # "docs/specs/example.md": "generated reference, linked from the API docs instead",
}

# Documents that predate the 600-line hard limit. docs/README.md section 0 says an existing
# oversized document is split when it is next edited; these are reported as warnings until then.
SIZE_ALLOW_LIST: Dict[str, str] = {
    "docs/specs/jsx-subset-v1.md": "789 lines when the gate landed; split when next edited",
}

# Broken links that are expected while an in-flight lane's doc lands. Entries are warnings, not
# errors; a stale entry (target now resolves) is reported so the exemption gets removed.
LINK_ALLOW_LIST: Dict[Tuple[str, str], str] = {
    # ("docs/specs/example.md", "../design/example.md"): "owned by the in-flight example lane",
}

EXTERNAL_RE = re.compile(r"^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//)")
INLINE_LINK_RE = re.compile(
    r"!?\[[^\]]*\]\(\s*(<[^>]*>|[^)\s]+)(?:\s+[\"'][^\"']*[\"'])?\s*\)"
)
REFERENCE_DEFINITION_RE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(\S+)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
STATUS_RE = re.compile(r"^\s{0,3}[-*]?\s*Status:\s*\S")
INLINE_CODE_RE = re.compile(r"`[^`]*`")


@dataclass(frozen=True, order=True)
class Issue:
    file: str
    line: int
    severity: str  # "ERROR" or "WARN"
    message: str


@dataclass
class ParsedMarkdown:
    rel: str
    path: Path
    lines: List[str]
    headings: Dict[str, int]
    links: List[Tuple[str, int]]
    issues: List[Issue]
    adr_heading_line: Optional[int]
    adr_status_line: Optional[int]


def strip_inline_for_slug(text: str) -> str:
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("`", "")
    text = text.replace("**", "")
    return text.strip()


def slugify(text: str) -> str:
    """GitHub heading slug: lowercase, spaces -> '-', punctuation removed, underscores kept."""
    text = strip_inline_for_slug(text).lower().strip()
    # GitHub removes punctuation (anything not word character, hyphen or space) and then turns
    # every remaining space into a hyphen; it does not collapse consecutive hyphens. This is why
    # "Build & test" has the anchor "#build--test".
    text = re.sub(r"[^\w\- ]", "", text, flags=re.UNICODE)
    return text.replace(" ", "-")


def heading_slugs(lines: Sequence[str]) -> Dict[str, int]:
    """Map slug -> first line number, with GitHub-style -1/-2 suffixes for duplicates."""
    seen: Dict[str, int] = {}
    slugs: Dict[str, int] = {}
    fence: Optional[Tuple[str, int, int]] = None
    for index, line in enumerate(lines, start=1):
        if fence is not None:
            if is_closing_fence(line, fence):
                fence = None
            continue
        opening = match_opening_fence(line)
        if opening is not None:
            fence = (opening[0], opening[1], index)
            continue
        heading = HEADING_RE.match(line)
        if not heading:
            continue
        base = slugify(heading.group(2))
        count = seen.get(base, 0)
        slug = base if count == 0 else "{}-{}".format(base, count)
        seen[base] = count + 1
        slugs.setdefault(slug, index)
    return slugs


def match_opening_fence(line: str) -> Optional[Tuple[str, int]]:
    stripped = line.lstrip(" ")
    if len(line) - len(stripped) > 3:
        return None
    if stripped.startswith("```"):
        return ("`", len(stripped) - len(stripped.lstrip("`")))
    if stripped.startswith("~~~"):
        return ("~", len(stripped) - len(stripped.lstrip("~")))
    return None


def is_closing_fence(line: str, fence: Tuple[str, int, int]) -> bool:
    marker, length, _ = fence
    stripped = line.strip()
    if not stripped or stripped[0] != marker:
        return False
    run = len(stripped) - len(stripped.lstrip(marker))
    return run >= length and stripped[run:].strip() == ""


def extract_links(line: str) -> List[str]:
    line = INLINE_CODE_RE.sub("", line)
    links = [match.group(1) for match in INLINE_LINK_RE.finditer(line)]
    definition = REFERENCE_DEFINITION_RE.match(line)
    if definition:
        links.append(definition.group(1))
    return [link[1:-1] if link.startswith("<") and link.endswith(">") else link for link in links]


def parse_markdown(repo_root: Path, path: Path) -> ParsedMarkdown:
    rel = path.relative_to(repo_root).as_posix()
    issues: List[Issue] = []
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raw = path.read_bytes()
        line = raw[: error.start].count(b"\n") + 1
        issues.append(
            Issue(
                rel,
                line,
                "ERROR",
                "file is not valid UTF-8 (byte 0x{:02x})".format(raw[error.start]),
            )
        )
        text = raw.decode("utf-8", errors="replace")
    lines = text.splitlines()
    links: List[Tuple[str, int]] = []
    fence: Optional[Tuple[str, int, int]] = None
    first_non_empty: Optional[int] = None
    status_line: Optional[int] = None
    for index, line in enumerate(lines, start=1):
        if first_non_empty is None and line.strip():
            first_non_empty = index
        if fence is not None:
            if is_closing_fence(line, fence):
                fence = None
            continue
        opening = match_opening_fence(line)
        if opening is not None:
            fence = (opening[0], opening[1], index)
            continue
        if status_line is None and STATUS_RE.match(line):
            status_line = index
        for target in extract_links(line):
            links.append((target, index))
    if fence is not None:
        issues.append(
            Issue(rel, fence[2], "ERROR", "unclosed code fence (opened here)")
        )
    return ParsedMarkdown(
        rel=rel,
        path=path,
        lines=lines,
        headings=heading_slugs(lines),
        links=links,
        issues=issues,
        adr_heading_line=first_non_empty,
        adr_status_line=status_line,
    )


def is_inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def resolve_link(repo_root: Path, source: Path, target: str) -> Tuple[Optional[Path], Optional[str], bool]:
    """Return (target path, fragment, outside_repository) or (None, None, False) for non-local."""
    if EXTERNAL_RE.match(target):
        return None, None, False
    path_part, _, fragment = target.partition("#")
    if not path_part:
        return source, unquote(fragment), False
    path_part = unquote(path_part)
    if path_part.startswith("/"):
        candidate = (repo_root / path_part.lstrip("/")).resolve()
    else:
        candidate = (source.parent / path_part).resolve()
    return candidate, unquote(fragment), not is_inside(candidate, repo_root)


class DocsChecker:
    def __init__(self, repo_root: Path) -> None:
        self.repo_root = repo_root
        self.parsed: Dict[Path, ParsedMarkdown] = {}
        self.issues: List[Issue] = []

    def add(self, file: str, line: int, severity: str, message: str) -> None:
        self.issues.append(Issue(file, line, severity, message))

    def markdown_files(self) -> List[Path]:
        files: List[Path] = []
        docs_dir = self.repo_root / "docs"
        if docs_dir.is_dir():
            files.extend(sorted(docs_dir.rglob("*.md")))
        decisions_dir = self.repo_root / "decisions"
        if decisions_dir.is_dir():
            files.extend(sorted(decisions_dir.glob("*.md")))
        for rel in ROOT_MARKDOWN + EXTRA_MARKDOWN:
            candidate = self.repo_root / rel
            if candidate.is_file():
                files.append(candidate)
        unique: List[Path] = []
        seen: Set[Path] = set()
        for path in files:
            resolved = path.resolve()
            if resolved not in seen:
                seen.add(resolved)
                unique.append(path)
        return unique

    def parse_all(self) -> None:
        for path in self.markdown_files():
            parsed = parse_markdown(self.repo_root, path)
            self.parsed[path.resolve()] = parsed
            self.issues.extend(parsed.issues)
            self.check_size(parsed)
            self.check_fences_already_added(parsed)

    def check_fences_already_added(self, parsed: ParsedMarkdown) -> None:
        # Fence errors are emitted by parse_markdown; nothing else to do.
        return

    def check_size(self, parsed: ParsedMarkdown) -> None:
        rel = parsed.rel
        in_scope = rel.startswith("docs/") or rel.startswith("decisions/")
        count = len(parsed.lines)
        if not in_scope:
            if rel in SIZE_EXEMPT_ROOT and count > HARD_LIMIT:
                self.add(
                    rel,
                    1,
                    "WARN",
                    "{} lines exceeds the {} hard limit; {} is exempt (warning only)".format(
                        count, HARD_LIMIT, rel
                    ),
                )
            return
        if count > HARD_LIMIT:
            reason = SIZE_ALLOW_LIST.get(rel)
            if reason:
                self.add(
                    rel,
                    1,
                    "WARN",
                    "{} lines exceeds the {} hard limit (grandfathered: {})".format(
                        count, HARD_LIMIT, reason
                    ),
                )
            else:
                self.add(
                    rel,
                    1,
                    "ERROR",
                    "{} lines exceeds the {} hard limit; split it into a folder index".format(
                        count, HARD_LIMIT
                    ),
                )
        elif count > SOFT_LIMIT:
            self.add(
                rel,
                1,
                "WARN",
                "{} lines exceeds the {} line target".format(count, SOFT_LIMIT),
            )

    def check_links(self) -> None:
        used_exemptions: Set[Tuple[str, str]] = set()
        for path, parsed in self.parsed.items():
            for target, line in parsed.links:
                resolved, fragment, outside = resolve_link(self.repo_root, path, target)
                if resolved is None and fragment is None:
                    continue
                if outside:
                    self.add(
                        parsed.rel,
                        line,
                        "WARN",
                        "link '{}' points outside the repository".format(target),
                    )
                    continue
                assert resolved is not None
                if not resolved.exists():
                    reason = LINK_ALLOW_LIST.get((parsed.rel, target))
                    if reason:
                        used_exemptions.add((parsed.rel, target))
                        self.add(
                            parsed.rel,
                            line,
                            "WARN",
                            "broken link '{}' allow-listed: {}".format(target, reason),
                        )
                    else:
                        self.add(
                            parsed.rel,
                            line,
                            "ERROR",
                            "broken relative link '{}' (resolved '{}' does not exist)".format(
                                target, self.rel_of(resolved)
                            ),
                        )
                    continue
                if resolved.is_dir():
                    self.add(
                        parsed.rel,
                        line,
                        "WARN",
                        "link '{}' points to a directory, not a file".format(target),
                    )
                    continue
                if not fragment:
                    continue
                target_parsed = self.parsed.get(resolved)
                if target_parsed is None or resolved.suffix.lower() != ".md":
                    self.add(
                        parsed.rel,
                        line,
                        "ERROR",
                        "link '{}' has a fragment but the target is not a checked markdown "
                        "file".format(target),
                    )
                    continue
                if fragment not in target_parsed.headings:
                    self.add(
                        parsed.rel,
                        line,
                        "ERROR",
                        "link '{}': target '{}' has no heading with anchor '#{}'".format(
                            target, self.rel_of(resolved), fragment
                        ),
                    )
        for rel_file, target in sorted(LINK_ALLOW_LIST):
            if (rel_file, target) not in used_exemptions:
                self.add(
                    rel_file,
                    1,
                    "WARN",
                    "stale link exemption for '{}': it now resolves or is gone; remove it from "
                    "LINK_ALLOW_LIST".format(target),
                )

    def rel_of(self, path: Path) -> str:
        return path.relative_to(self.repo_root).as_posix()

    def check_index(self) -> None:
        docs_dir = (self.repo_root / "docs").resolve()
        index_path = (docs_dir / "README.md").resolve()
        docs_files = [
            path
            for path in self.parsed
            if is_inside(path, docs_dir) and path.suffix.lower() == ".md"
        ]
        reachable: Set[Path] = set()
        queue: List[Path] = []
        if index_path.is_file():
            reachable.add(index_path)
            queue.append(index_path)
        else:
            if docs_files:
                self.add("docs/README.md", 1, "ERROR", "documentation map is missing")
        while queue:
            current = queue.pop(0)
            parsed = self.parsed.get(current)
            if parsed is None:
                continue
            for target, _ in parsed.links:
                resolved, _, outside = resolve_link(self.repo_root, current, target)
                if resolved is None or outside or not resolved.is_file():
                    continue
                if resolved.suffix.lower() != ".md" or not is_inside(resolved, docs_dir):
                    continue
                if resolved not in reachable:
                    reachable.add(resolved)
                    if resolved.name == "README.md":
                        queue.append(resolved)
        for path in docs_files:
            if path in reachable:
                continue
            rel = self.rel_of(path)
            reason = INDEX_ALLOW_LIST.get(rel)
            if reason:
                self.add(rel, 1, "WARN", "not in the docs map (allow-listed: {})".format(reason))
            else:
                self.add(
                    rel,
                    1,
                    "ERROR",
                    "not referenced from docs/README.md (directly or via a folder README)",
                )
        for rel, _reason in sorted(INDEX_ALLOW_LIST.items()):
            path = (self.repo_root / rel).resolve()
            if path.is_file() and path in reachable:
                self.add(
                    rel,
                    1,
                    "WARN",
                    "stale index exemption: the doc is now in the docs map; remove it from "
                    "INDEX_ALLOW_LIST",
                )

    def check_decisions(self) -> None:
        decisions_dir = self.repo_root / "decisions"
        index_path = (decisions_dir / "README.md").resolve()
        adr_files = sorted(decisions_dir.glob("ADR-*.md"))
        linked: Set[Path] = set()
        if index_path.is_file():
            parsed = self.parsed.get(index_path)
            if parsed is not None:
                for target, _ in parsed.links:
                    resolved, _, outside = resolve_link(self.repo_root, index_path, target)
                    if resolved is not None and not outside:
                        linked.add(resolved)
        else:
            if adr_files:
                self.add("decisions/README.md", 1, "ERROR", "ADR index is missing")
        for path in adr_files:
            rel = self.rel_of(path)
            parsed = self.parsed.get(path.resolve())
            if path.resolve() not in linked:
                self.add(rel, 1, "ERROR", "ADR is not listed in decisions/README.md")
            if parsed is None:
                continue
            if parsed.adr_heading_line is None:
                self.add(rel, 1, "ERROR", "ADR is empty")
            else:
                first_line = parsed.lines[parsed.adr_heading_line - 1]
                if not first_line.startswith("# "):
                    self.add(
                        rel,
                        parsed.adr_heading_line,
                        "ERROR",
                        "ADR must start with an '# ' heading (found {!r})".format(
                            first_line.strip()[:60]
                        ),
                    )
            if parsed.adr_status_line is None or parsed.adr_status_line > 15:
                self.add(
                    rel,
                    1,
                    "ERROR",
                    "ADR is missing a '- Status:' line near the top",
                )

    def run(self) -> List[Issue]:
        self.parse_all()
        self.check_links()
        self.check_index()
        self.check_decisions()
        return sorted(self.issues, key=lambda issue: (issue.file, issue.line, issue.severity, issue.message))

    def file_count(self) -> int:
        return len(self.parsed)


def format_issue(issue: Issue) -> str:
    return "{} {}:{} {}".format(issue.severity, issue.file, issue.line, issue.message)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--repo-root",
        default=str(REPO_ROOT),
        help="repository root to check (default: the checkout containing this script)",
    )
    parser.add_argument("--json", action="store_true", dest="as_json", help="machine-readable report")
    args = parser.parse_args(argv)

    repo_root = Path(args.repo_root).resolve()
    if not repo_root.is_dir():
        print("error: repository root not found: {}".format(repo_root), file=sys.stderr)
        return 2

    checker = DocsChecker(repo_root)
    issues = checker.run()
    errors = [issue for issue in issues if issue.severity == "ERROR"]
    warnings = [issue for issue in issues if issue.severity == "WARN"]

    if args.as_json:
        payload = {
            "ok": not errors,
            "gate": "docs",
            "files_checked": checker.file_count(),
            "errors": [issue.__dict__ for issue in errors],
            "warnings": [issue.__dict__ for issue in warnings],
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        for issue in issues:
            print(format_issue(issue))
        print(
            "docs gate: {} error(s), {} warning(s), {} markdown file(s) checked".format(
                len(errors), len(warnings), checker.file_count()
            )
        )
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
