#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate `designer/web/public/samples.json`, the sample gallery's manifest (issue #169, stage P5).

Stage P5 of `docs/design/reference-equantic-ui.md` section 7.4 asks for "a sample gallery built from
`samples/` -- a manifest the page reads, so adding a sample does not mean editing JS". This script is
the thing that keeps that promise true:

    python -B scripts/ci/gen_sample_manifest.py            # write the manifest
    python -B scripts/ci/gen_sample_manifest.py --check    # fail if the checked-in copy is stale

**The TSX is the single source of truth.** The manifest stores a *path* and the exported component
name, never a copy of the source: the page fetches `samples/...` at run time, exactly as a person
would read the file. Nothing in the page source lists a sample, so adding `samples/new/Thing.tsx`
and re-running this script is the whole procedure; a stale manifest fails `--check` in CI, which is
what stops the gallery from silently missing a sample.

**Why a committed artifact rather than a build step.** `designer/web` is deployed as static files by
`.github/workflows/pages.yml` with no page build (issue #165). Generating the manifest at deploy time
would make it a build output that no reviewer sees in the diff; committing it and gating on drift
keeps the Pages job a pure copy and keeps the gallery's contents reviewable.

**Display titles are not in the file.** They are derived in the page from the file stem
(`SafeAreaPage.tsx` -> "Safe Area Page"), so the manifest carries no prose that could drift from the
file it describes, and a rename cannot leave a stale title behind.

The manifest is also a *budget* input: it is one of the files `scripts/check_preview_budget.py`
weighs (see `PAGE_FILES` there), which is why the format is as small as it can be while staying
readable.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent.parent

# Overridable so the self-tests can run the real script against a throwaway tree; a root that came
# from `__file__` alone would make the generator untestable and would break if the tree were moved.
SAMPLES_DIR = REPO_ROOT / "samples"
MANIFEST = REPO_ROOT / "designer" / "web" / "public" / "samples.json"

# The page reads schema 1; a newer one is a page change, not a manifest edit.
SCHEMA = 1

# `export function Name(` or `export const Name =`, the two shapes the samples use. The front end
# accepts more (default exports, arrow components without a name), but a sample whose entry point
# this cannot name is a sample the gallery cannot open, so it is reported rather than skipped.
EXPORT = re.compile(r"^\s*export\s+(?:async\s+)?(?:function|const|class)\s+([A-Za-z_$][\w$]*)", re.M)


class ManifestError(Exception):
    """A sample cannot be described, or the manifest itself is unusable."""


def relative(path: Path, root: Path) -> str:
    """A repository-relative POSIX path, or the file name when it sits outside `root`."""
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.name


def entry_component(text: str, path: Path, root: Path) -> str:
    """The component the gallery previews: the first exported, capitalized declaration in the file.

    Capitalized because that is the JSX convention the compiler enforces for a component; a lower-case
    export is a helper, not a screen.
    """
    for match in EXPORT.finditer(text):
        name = match.group(1)
        if name[:1].isupper():
            return name
    raise ManifestError(
        "{} exports no capitalized component; the gallery cannot open it".format(relative(path, root))
    )


def sample_entries(root: Path) -> List[dict]:
    """Every `.tsx` under `samples/`, sorted by path so the manifest is reproducible."""
    samples_dir = root / "samples"
    if not samples_dir.is_dir():
        raise ManifestError("no samples/ directory at {}".format(samples_dir))
    entries: List[dict] = []
    errors: List[str] = []
    for path in sorted(samples_dir.rglob("*.tsx")):
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        try:
            component = entry_component(text, path, root)
        except ManifestError as exc:
            errors.append(str(exc))
            continue
        entries.append({"path": relative(path, root), "component": component})
    if errors:
        raise ManifestError("\n".join(errors))
    if not entries:
        # An empty gallery is a page with an empty pane and no error, which reads as "the feature is
        # broken" rather than "there are no samples". Refuse instead.
        raise ManifestError("samples/ contains no .tsx files; the gallery would be empty")
    return entries


def render(entries: List[dict]) -> str:
    """The manifest bytes: stable key order, one entry per line, trailing newline."""
    # One sample per line so a diff names the sample that changed instead of reformatting the file.
    body = ",\n".join(
        "    " + json.dumps(entry, ensure_ascii=False, sort_keys=True) for entry in entries
    )
    return '{{\n  "schema": {},\n  "samples": [\n{}\n  ]\n}}\n'.format(SCHEMA, body)


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the checked-in manifest matches samples/ instead of writing it",
    )
    parser.add_argument(
        "--manifest", default=str(MANIFEST), help="manifest path (default: designer/web/public/samples.json)"
    )
    parser.add_argument(
        "--repo-root",
        default=str(REPO_ROOT),
        help="the tree to scan for samples/ (default: this checkout; the self-tests use a temp tree)",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    root = Path(args.repo_root)
    manifest = Path(args.manifest)
    try:
        entries = sample_entries(root)
    except ManifestError as exc:
        print("SETUP ERROR: {}".format(exc), file=sys.stderr)
        return 2

    expected = render(entries)
    if args.check:
        if not manifest.is_file():
            print(
                "ERROR: {} is missing; run python -B scripts/ci/gen_sample_manifest.py".format(
                    relative(manifest, root)
                ),
                file=sys.stderr,
            )
            return 1
        actual = manifest.read_text(encoding="utf-8")
        if actual != expected:
            print(
                "ERROR: {} is stale; run python -B scripts/ci/gen_sample_manifest.py".format(
                    relative(manifest, root)
                ),
                file=sys.stderr,
            )
            # Name the difference, because "stale" alone makes a reviewer diff it by hand.
            try:
                known = {
                    entry["path"]
                    for entry in json.loads(actual).get("samples", [])
                    if isinstance(entry, dict) and "path" in entry
                }
            except (json.JSONDecodeError, AttributeError):
                print("  the checked-in file is not a schema-1 manifest", file=sys.stderr)
                known = set()
            current = {entry["path"] for entry in entries}
            for path in sorted(current - known):
                print("  missing: {}".format(path), file=sys.stderr)
            for path in sorted(known - current):
                print("  removed: {}".format(path), file=sys.stderr)
            if current == known:
                print("  the sample set matches; the component name or format changed", file=sys.stderr)
            return 1
        print("sample manifest: {} sample(s), up to date".format(len(entries)))
        return 0

    manifest.parent.mkdir(parents=True, exist_ok=True)
    try:
        manifest.write_text(expected, encoding="utf-8")
    except OSError as exc:
        print(
            "SETUP ERROR: cannot write {}: {}".format(relative(manifest, root), exc), file=sys.stderr
        )
        return 2
    print(
        "sample manifest written: {} sample(s) in {}".format(
            len(entries), relative(manifest, root)
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
