#!/usr/bin/env python3
"""Tests for `parity_gate.py` (issue #529).

**What these can and cannot cover.** The gate's job is to turn a comparison number into an exit code,
and that decision is testable without a browser: a report over the threshold fails, one under it
passes, a step that cannot run is not a pass. The browser half cannot be tested here - Playwright is
not a repository dependency, and a test that needed Chrome would be the CI gate the issue says this
must not become.

So the tests drive `main()` with the subprocess layer replaced: `run()` answers as the real tools
would, including writing the artifacts, and `check_playwright` is stubbed to "available" unless a
test is about its absence. That keeps every assertion about the gate's own logic - thresholds, exit
codes, the artifact report, the cannot-run path - and none about somebody's browser.
"""

from __future__ import annotations

import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

# Load the script as a module. It lives in `scripts/ci/` and has no package, so the path is explicit.
_SPEC = importlib.util.spec_from_file_location(
    "parity_gate", Path(__file__).resolve().parent / "parity_gate.py"
)
parity_gate = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(parity_gate)


class FakeTools:
    """Stands in for `run()`, answering by the program it is asked to run.

    It writes the files the real tools write, because the gate checks for them: a capture that
    returns 0 without a PNG is the silent failure the gate exists to catch, so the fake must be able
    to reproduce both the honest and the silent case.
    """

    def __init__(self, *, structure: float, pixels: float = 3.9, capture_writes: bool = True):
        self.structure = structure
        self.pixels = pixels
        self.capture_writes = capture_writes
        self.calls: list[list[str]] = []

    def __call__(self, command, *, cwd):
        self.calls.append([str(part) for part in command])
        # The browser and comparison steps run as `<interpreter> <script> ...`, so the program is a
        # later element when the first is an interpreter. Dispatch on the first element this fake
        # knows, which keeps it independent of how many flags an interpreter is given.
        known = {"gallery", "gallery.exe", "chrome_parity.mjs", "chrome_parity_compare.py"}
        program = next(
            (Path(part).name for part in command if Path(part).name in known), Path(command[0]).name
        )
        # The gallery: `--capture` writes engine.png, `--emit-module-css` writes nothing.
        if program in ("gallery", "gallery.exe"):
            if "--capture" in command and self.capture_writes:
                out = Path(command[command.index("--capture") + 1])
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(b"\x89PNG engine")
            return subprocess_result(0)
        # The browser half writes its capture and the boxes file.
        if program == "chrome_parity.mjs":
            out = Path(command[command.index("--out") + 1])
            (out / "chrome.png").write_bytes(b"\x89PNG chrome")
            (out / "chrome-boxes.json").write_text("[]", encoding="utf-8")
            return subprocess_result(0)
        # The comparison writes report.json and the artifact images.
        if program == "chrome_parity_compare.py":
            out = Path(command[command.index("--dir") + 1])
            (out / "report.json").write_text(
                json.dumps(
                    {
                        "structure_diff_pct": self.structure,
                        "differing_pixels_pct": self.pixels,
                    }
                ),
                encoding="utf-8",
            )
            for name in ("side_by_side.png", "diff.png", "overlay.png"):
                (out / name).write_bytes(b"\x89PNG artifact")
            return subprocess_result(0)
        raise AssertionError(f"unexpected command: {command}")


def subprocess_result(code: int, stdout: str = "", stderr: str = ""):
    """A `CompletedProcess` with the fields the gate reads."""

    class Result:
        returncode = code

    result = Result()
    result.returncode = code
    result.stdout = stdout
    result.stderr = stderr
    return result


class ParityGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        # A gallery binary the gate's finder will accept.
        self.gallery = self.repo / "build" / "dev" / "samples" / "gallery" / "gallery"
        self.gallery.parent.mkdir(parents=True)
        self.gallery.write_bytes(b"#!/bin/sh\n")
        # The two script names `find_gallery` and the browser step look for.
        (self.repo / "scripts" / "ci").mkdir(parents=True)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def gate(self, tools: FakeTools, *argv: str, playwright_ok: bool = True):
        """Runs `main()` with the tools faked, returning (exit code, stdout, stderr)."""
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(parity_gate, "run", tools), mock.patch.object(
            parity_gate, "check_playwright", lambda repo: None if playwright_ok else "no playwright"
        ), mock.patch.object(
            parity_gate.shutil, "which", lambda name: "/usr/bin/node" if name == "node" else None
        ), mock.patch.object(
            sys, "argv", ["parity_gate.py", *argv]
        ), redirect_stdout(out), redirect_stderr(err):
            code = parity_gate.main()
        return code, out.getvalue(), err.getvalue()

    def test_within_the_threshold_passes(self) -> None:
        # The form is at 0.12 %; the threshold is 0.5 %.
        code, out, _ = self.gate(FakeTools(structure=0.12), "--page", "form", "--repo", str(self.repo))
        self.assertEqual(code, 0, out)
        self.assertIn("structure_diff_pct=0.12", out)
        self.assertIn("PASS", out)

    def test_over_the_threshold_fails(self) -> None:
        code, _, err = self.gate(FakeTools(structure=0.62), "--page", "form", "--repo", str(self.repo))
        self.assertEqual(code, 1, "a structure difference over the threshold must fail the gate")
        self.assertIn("over the 0.50", err)

    def test_exactly_at_the_threshold_passes(self) -> None:
        # `<=`, not `<`: the threshold is a maximum, and a number that lands exactly on it is within
        # it. Worth pinning because an off-by-one here flips a green page red for no reason.
        code, _, _ = self.gate(FakeTools(structure=0.5), "--page", "form", "--repo", str(self.repo))
        self.assertEqual(code, 0)

    def test_pixels_are_reported_but_not_gated(self) -> None:
        # A huge pixel difference with a tiny structure difference must still pass: the pixels are
        # text anti-aliasing, and gating them would gate the font rather than the component.
        code, out, _ = self.gate(
            FakeTools(structure=0.1, pixels=42.0), "--page", "form", "--repo", str(self.repo)
        )
        self.assertEqual(code, 0)
        self.assertIn("differing_pixels_pct=42.00", out, "the number is still reported")

    def test_a_plain_css_page_skips_the_module_step(self) -> None:
        # `dash` has no CSS module, so `--emit-module-css` must not be invoked for it.
        tools = FakeTools(structure=0.1)
        code, _, _ = self.gate(tools, "--page", "dash", "--repo", str(self.repo))
        self.assertEqual(code, 0)
        emitted = [call for call in tools.calls if "--emit-module-css" in call]
        self.assertEqual(emitted, [], "dash must not be asked for the form's module")

    def test_the_engine_is_handed_the_font_pack_face(self) -> None:
        # #970: the capture uses the bundled Plex Sans, the same file the browser half loads.
        tools = FakeTools(structure=0.1)
        code, _, _ = self.gate(tools, "--page", "form", "--repo", str(self.repo))
        self.assertEqual(code, 0)
        captures = [call for call in tools.calls if "--capture" in call]
        self.assertEqual(len(captures), 1)
        call = captures[0]
        fonts = [Path(call[i + 1]) for i, arg in enumerate(call) if arg == "--font"]
        # #973: the regular face first (the primary), then the weights the engine picks per run.
        self.assertEqual(
            [f.name for f in fonts],
            ["IBMPlexSans-Regular.ttf", "IBMPlexSans-SemiBold.ttf", "IBMPlexSans-Bold.ttf"],
        )
        for font in fonts:
            self.assertIn("tessera-fonts", font.parts)

    def test_a_module_page_emits_first(self) -> None:
        tools = FakeTools(structure=0.1)
        code, _, _ = self.gate(tools, "--page", "form", "--repo", str(self.repo))
        self.assertEqual(code, 0)
        emitted = [call for call in tools.calls if "--emit-module-css" in call]
        self.assertEqual(len(emitted), 1, "form is a CSS module and needs its scoped names first")
        # And the browser half is handed the same directory, or it would use unscoped names.
        browser = [call for call in tools.calls if any("chrome_parity.mjs" in part for part in call)]
        self.assertEqual(len(browser), 1)
        self.assertIn("--module-css", browser[0])

    def test_the_browser_half_is_told_which_page(self) -> None:
        # #973: without --page chrome_parity.mjs defaulted to the form, so `--page dash` compared the
        # dashboard's engine capture against the form's twin.
        tools = FakeTools(structure=0.1)
        code, _, _ = self.gate(tools, "--page", "dash", "--repo", str(self.repo))
        self.assertEqual(code, 0)
        browser = [call for call in tools.calls if any("chrome_parity.mjs" in part for part in call)]
        self.assertEqual(browser[0][browser[0].index("--page") + 1], "dash")

    def test_missing_playwright_is_one_actionable_line(self) -> None:
        code, _, err = self.gate(FakeTools(structure=0.1), "--page", "form", "--repo", str(self.repo), playwright_ok=False)
        self.assertEqual(code, 2, "a step that cannot run is not a pass")
        self.assertIn("parity_gate:", err)
        self.assertNotIn("Traceback", err, "the issue asks for one line, not a stack trace")
        self.assertEqual(len(err.strip().splitlines()), 1)

    def test_a_capture_that_writes_nothing_is_not_a_pass(self) -> None:
        # The guide's manual steps could not catch this: a device without Vulkan prints "skipped" and
        # returns 0 without a PNG. A gate must not read that as success.
        code, _, err = self.gate(
            FakeTools(structure=0.1, capture_writes=False), "--page", "form", "--repo", str(self.repo)
        )
        self.assertEqual(code, 2)
        self.assertIn("produced no", err)

    def test_the_artifacts_are_named(self) -> None:
        # A farm agent attaches these to a PR, so the gate has to say where they are.
        code, out, _ = self.gate(FakeTools(structure=0.1), "--page", "form", "--repo", str(self.repo))
        self.assertEqual(code, 0)
        for name in ("report.json", "side_by_side.png", "diff.png", "overlay.png"):
            self.assertIn(name, out, f"the gate reports {name}")

    def test_an_unknown_page_is_refused_by_argparse(self) -> None:
        # The page set is the gate's own knowledge, so a page it does not know is refused before any
        # tool runs rather than silently gating nothing.
        with mock.patch.object(sys, "argv", ["parity_gate.py", "--page", "nope", "--repo", str(self.repo)]):
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                parity_gate.main()
        self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
