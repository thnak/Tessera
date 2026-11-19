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

**A sample is a small project, not one file (schema 2).** `samples/gallery/Gallery.tsx` imports
`./Button`; the native build satisfies that by copying `packages/tessera-ui/Button.tsx` next to the
sample before codegen. The Studio must hand the compiler the same project, so each entry carries
`files: [{from, as}]`: the companion files (repository path -> project-relative path) the page fetches
and opens next to the entry. They are **derived**, never listed by hand: every relative import
(`./X`, recursively, stylesheets and CSS modules included) resolves to a sibling file in the sample's
directory first and else to `packages/tessera-ui/X`, the rule the CMake copies encode. An import that
resolves to neither is a hard generator failure, so a new sample with a dangling import fails
`--check` in CI instead of showing "source not applied" in the Studio.

The manifest is also a *budget* input: it is one of the files `scripts/check_preview_budget.py`
weighs (see `PAGE_FILES` there), which is why the format is as small as it can be while staying
readable.
"""

from __future__ import annotations

import argparse
import json
import posixpath
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

# The page reads exactly this schema; a newer one is a page change, not a manifest edit.
# Schema 2 added `files` (the companion files of a sample, see the module docstring).
SCHEMA = 2

# Where a relative import falls back to when the sample's own directory does not have the file: the
# component library the native build copies next to the sample (samples/gallery/CMakeLists.txt).
LIBRARY_DIR = "packages/tessera-ui"

# `export function Name(` or `export const Name =`, the two shapes the samples use. The front end
# accepts more (default exports, arrow components without a name), but a sample whose entry point
# this cannot name is a sample the gallery cannot open, so it is reported rather than skipped.
EXPORT = re.compile(r"^\s*export\s+(?:async\s+)?(?:function|const|class)\s+([A-Za-z_$][\w$]*)", re.M)


# A relative import or re-export specifier: `import {A} from "./X"`, `export {A} from "./X"` and the
# side-effect form `import "./x.css"`. Comments are blanked first so prose cannot name a file.
RELATIVE_IMPORT = re.compile(
    r"""(?:\b(?:import|export)\b[^;"']*?\bfrom\s*|\bimport\s*)["'](\.{1,2}/[^"']*)["']""", re.S
)
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"(?m)^\s*//.*$")


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


def relative_imports(text: str) -> List[str]:
    """The relative specifiers `text` imports or re-exports, in source order, comments ignored."""
    code = LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", text))
    return [match.group(1) for match in RELATIVE_IMPORT.finditer(code)]


def import_target(specifier: str) -> str:
    """The file name a specifier names: a bare `./Button` is the `.tsx` module, `./x.css` is itself."""
    return specifier if re.search(r"\.[A-Za-z0-9]+$", specifier) else specifier + ".tsx"


def companion_files(entry: Path, root: Path) -> List[dict]:
    """The `{from, as}` companions of one sample: everything its relative imports reach, recursively.

    `as` is project-relative and sits under the entry's own directory (the Studio opens the sample at
    its repository path, so `./Button` resolves exactly as it does in the native build's flat source
    directory).
    """
    base = relative(entry, root)
    base_dir = posixpath.dirname(base)
    found: dict = {}
    seen = {base}
    pending = [(base, entry.read_text(encoding="utf-8", errors="replace"))]
    errors: List[str] = []
    while pending:
        importer, text = pending.pop()
        for specifier in relative_imports(text):
            target = posixpath.normpath(
                posixpath.join(posixpath.dirname(importer), import_target(specifier))
            )
            inside = posixpath.relpath(target, base_dir)
            if inside.startswith(".."):
                errors.append(
                    "{}: import `{}` leaves the sample's directory".format(importer, specifier)
                )
                continue
            if target in seen:
                continue
            sibling = root / target
            library = root / LIBRARY_DIR / inside
            if sibling.is_file():
                source = sibling
            elif library.is_file():
                source = library
            else:
                errors.append(
                    "{}: import `{}` resolves to neither {} nor {}/{}".format(
                        importer, specifier, target, LIBRARY_DIR, inside
                    )
                )
                continue
            seen.add(target)
            found[target] = relative(source, root)
            if target.endswith((".tsx", ".ts")):
                pending.append((target, source.read_text(encoding="utf-8", errors="replace")))
    if errors:
        raise ManifestError("\n".join(sorted(errors)))
    return [{"as": name, "from": found[name]} for name in sorted(found)]


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
        try:
            files = companion_files(path, root)
        except ManifestError as exc:
            errors.append(str(exc))
            continue
        entries.append({"path": relative(path, root), "component": component, "files": files})
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
                print(
                    "  the checked-in file is not a schema-{} manifest".format(SCHEMA), file=sys.stderr
                )
                known = set()
            current = {entry["path"] for entry in entries}
            for path in sorted(current - known):
                print("  missing: {}".format(path), file=sys.stderr)
            for path in sorted(known - current):
                print("  removed: {}".format(path), file=sys.stderr)
            if current == known:
                print("  the sample set matches; the component name, companion files or format changed", file=sys.stderr)
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
