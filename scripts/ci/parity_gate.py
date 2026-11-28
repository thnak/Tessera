#!/usr/bin/env python3
"""One command that gates a gallery page against Chrome (issue #529).

**Why this exists.** `docs/guides/chrome-parity.md` documented four manual steps, and the result was
a `report.json` a human read. Nothing exited non-zero on a regression, so "this component matches
Chrome" was a judgement rather than a gate - and a per-component definition of done only works if one
command answers pass or fail.

**What it gates.** `structure_diff_pct`, the share of elements whose box differs beyond the
tolerance, against a threshold. `differing_pixels_pct` is printed and recorded but **not** gated: it
is dominated by text anti-aliasing, so gating it would fail on the font rather than on the component
(the guide says the same).

**What it is not.** It is not a CI gate. It needs a browser and a lavapipe capture, so it stays a
developer and farm tool, like the guide it replaces - and a lane that ran it on every PR would need
Chrome on that lane.

Usage:
    python -B scripts/ci/parity_gate.py --page form [--threshold 0.5] [--out out/parity]
                                        [--gallery build/dev/samples/gallery/gallery]

Exit codes: 0 within the threshold, 1 over it, 2 when a step could not run (missing Playwright, a
missing gallery binary, a missing capture). A step that cannot run is not a pass, which is the whole
point of replacing a judgement.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

# The threshold the issue sets: structure difference under 0.5 %, the form being at 0.12 %.
DEFAULT_THRESHOLD = 0.5

# The default capture size the guide uses for the form. A page that needs another passes --width and
# --height; nothing here guesses a page's size from its name.
DEFAULT_WIDTH = 640
DEFAULT_HEIGHT = 720

# The fonts both halves are handed: IBM Plex Sans from the bundled pack (#970), the family the
# Instrument theme names, so the gate no longer measures a stand-in. The first is the primary face; the
# engine picks the others per run by the weight each file declares (repeatable `--font`, #973), and
# chrome_parity.mjs declares the same three files as 400, 600 and 700 so neither side synthesizes bold.
PARITY_FONT = "packages/tessera-fonts/ibm-plex/fonts/IBMPlexSans-Regular.ttf"
PARITY_FONT_FACES = (
    PARITY_FONT,
    "packages/tessera-fonts/ibm-plex/fonts/IBMPlexSans-SemiBold.ttf",
    "packages/tessera-fonts/ibm-plex/fonts/IBMPlexSans-Bold.ttf",
)

# Exit codes, named so the messages and the returns cannot drift.
EXIT_PASS = 0
EXIT_OVER_THRESHOLD = 1
EXIT_CANNOT_RUN = 2

# The pages this command knows, and whether each needs `--emit-module-css` first.
#
# `form` is a CSS module (issue #463): its class names are scoped at compile time, so the browser twin
# has to be handed the same scoped names the engine used. `dash` is plain CSS. A page that needs a
# step this table does not describe is a page that must be added here, not guessed at.
PAGES = {
    "form": {"module_css": True},
    "dash": {"module_css": False},
}


def fail(message: str) -> int:
    """One actionable line, then the cannot-run exit code.

    The issue asks for exactly this: a missing Playwright or Chrome produces one line a developer can
    act on, not a stack trace.
    """
    print(f"parity_gate: {message}", file=sys.stderr)
    return EXIT_CANNOT_RUN


def run(command: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    """Runs `command`, capturing its output so a failure reports what it said."""
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)


def find_gallery(explicit: str | None, repo: Path) -> Path | None:
    """The gallery binary: the one given, else the first build directory that has one.

    The build directory is a preset name (`build/dev`, `build/ci`, `build/vs2026`), so searching is
    better than asking for a path the caller has to know.
    """
    if explicit:
        candidate = Path(explicit)
        return candidate if candidate.is_file() else None
    for preset in ("dev", "ci", "release", "vs2026", "vs2026-release"):
        for relative in ("samples/gallery/gallery", "samples/gallery/Release/gallery.exe"):
            candidate = repo / "build" / preset / relative
            if candidate.is_file():
                return candidate
    return None


def check_playwright(repo: Path) -> str | None:
    """`None` when the browser half can run, else the reason in one line.

    Playwright is deliberately not a repository dependency (`docs/guides/chrome-parity.md`), so the
    check is "is it importable from `scripts/ci`", which is where the repository's Node tooling lives.
    """
    node = shutil.which("node")
    if node is None:
        return "node is not on PATH; the browser half needs it (see docs/guides/chrome-parity.md)"
    probe = run([node, "-e", "require.resolve('playwright')"], cwd=repo / "scripts" / "ci")
    if probe.returncode != 0:
        return (
            "playwright is not installed for scripts/ci; run "
            "`npm ci --prefix scripts/ci` (see docs/guides/chrome-parity.md)"
        )
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Gate a gallery page's structure against Chrome.")
    parser.add_argument("--page", required=True, choices=sorted(PAGES), help="the gallery page to gate")
    parser.add_argument(
        "--threshold",
        type=float,
        default=DEFAULT_THRESHOLD,
        help=f"maximum structure_diff_pct (default {DEFAULT_THRESHOLD})",
    )
    parser.add_argument("--out", default="out/parity", help="artifact directory (default out/parity)")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--gallery", default=None, help="the gallery binary, if not under build/")
    parser.add_argument("--repo", default=None, help="the repository root (default: derived)")
    args = parser.parse_args()

    repo = Path(args.repo).resolve() if args.repo else Path(__file__).resolve().parents[2]
    out = (repo / args.out).resolve()
    page = PAGES[args.page]

    missing = check_playwright(repo)
    if missing is not None:
        return fail(missing)

    gallery = find_gallery(args.gallery, repo)
    if gallery is None:
        return fail(
            "no gallery binary found under build/; build one "
            "(`cmake --build build/dev --target gallery`) or pass --gallery"
        )

    out.mkdir(parents=True, exist_ok=True)

    # 1. The engine's capture. `--emit-module-css` is a separate invocation because the flag writes
    #    files rather than painting: the module's scoped names must exist before either the engine's
    #    page or the browser's twin can style itself from them.
    if page["module_css"]:
        emit = run([str(gallery), "--page", args.page, "--emit-module-css", str(out)], cwd=repo)
        if emit.returncode != 0:
            return fail(f"`gallery --emit-module-css` failed: {emit.stderr.strip() or emit.stdout.strip()}")

    capture = run(
        [
            str(gallery),
            "--page",
            args.page,
            "--capture",
            str(out / "engine.png"),
            *[arg for face in PARITY_FONT_FACES for arg in ("--font", str(repo / face))],
            "--width",
            str(args.width),
            "--height",
            str(args.height),
        ],
        cwd=repo,
    )
    if capture.returncode != 0:
        return fail(f"`gallery --capture` failed: {capture.stderr.strip() or capture.stdout.strip()}")
    if not (out / "engine.png").is_file():
        # A capture that returns 0 without writing is the one silent failure the guide's manual steps
        # could hide; a gate must not.
        return fail(f"`gallery --capture` produced no {out / 'engine.png'} (a device without Vulkan prints 'skipped')")

    # 2. The browser twin. It writes chrome.png and chrome-boxes.json into the same directory.
    node = shutil.which("node")
    browser_cmd = [
        node,
        str(repo / "scripts" / "ci" / "chrome_parity.mjs"),
        "--page",
        args.page,
        "--out",
        str(out),
        "--width",
        str(args.width),
        "--height",
        str(args.height),
    ]
    if page["module_css"]:
        browser_cmd += ["--module-css", str(out)]
    browser = run(browser_cmd, cwd=repo)
    if browser.returncode != 0:
        return fail(f"the Chrome half failed: {browser.stderr.strip() or browser.stdout.strip()}")

    # 3. The comparison, which writes report.json and the three artifact images.
    compare = run(
        [sys.executable, "-B", str(repo / "scripts" / "ci" / "chrome_parity_compare.py"), "--dir", str(out)],
        cwd=repo,
    )
    if compare.returncode != 0:
        return fail(f"the comparison failed: {compare.stderr.strip() or compare.stdout.strip()}")

    report_path = out / "report.json"
    if not report_path.is_file():
        return fail(f"the comparison wrote no {report_path}")
    report = json.loads(report_path.read_text(encoding="utf-8"))

    structure = float(report.get("structure_diff_pct", 0.0))
    pixels = float(report.get("differing_pixels_pct", 0.0))
    within = structure <= args.threshold

    print(f"parity_gate: page={args.page} structure_diff_pct={structure:.2f} differing_pixels_pct={pixels:.2f}")
    print(f"parity_gate: threshold={args.threshold:.2f} -> {'PASS' if within else 'FAIL'}")
    # The artifact paths, so a farm agent can attach them to a PR without knowing where the script
    # put them.
    for name in ("report.json", "side_by_side.png", "diff.png", "overlay.png"):
        candidate = out / name
        if candidate.is_file():
            print(f"parity_gate: {name}: {candidate}")

    if not within:
        print(
            f"parity_gate: structure differs by {structure:.2f} %, over the {args.threshold:.2f} % "
            f"threshold - see {out / 'diff.png'}",
            file=sys.stderr,
        )
        return EXIT_OVER_THRESHOLD
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
