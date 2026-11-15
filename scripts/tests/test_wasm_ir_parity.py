#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture tests for scripts/ci/wasm_ir_parity.py (issue #164).

Fake native and "Wasm" CLIs stand in for the two compilers, so the gate's pass, mismatch,
determinism, generated-C++ and setup-error semantics are covered without Emscripten or a build.
The fake CLI writes the IR file requested by ``--emit-ir`` and a tree for ``--write``;
a ``FAKE_*`` environment variable selects the behavior under test.
"""

from __future__ import annotations

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
CHECK = SCRIPTS_DIR / "ci" / "wasm_ir_parity.py"

SAMPLE_REL = "samples/parity-demo/Counter.tsx"
IR_BODY = '{\n  "name": "Counter"\n}\n'
# A second body the "Wasm" side emits when the fixture asks for a mismatch.
IR_BODY_DRIFTED = '{\n  "name": "CounterX"\n}\n'
FAKE_CLI = """#!{python}
import os
import sys
from pathlib import Path

args = sys.argv[1:]
# The side comes from the executable name: the gate runs both sides with the same environment, so
# an env var could not distinguish them.
side = "wasm" if "wasm" in Path(sys.argv[0]).stem.lower() else "native"
variant = os.environ.get("FAKE_VARIANT", "same")
if "--emit-ir" in args:
    target = Path(args[args.index("--emit-ir") + 1])
    body = {ir_body!r}
    if variant == "drift" and side == "wasm":
        body = {drifted!r}
    elif variant == "nondeterministic":
        # Both sides agree inside one run (the run directory name is the seed) but differ between
        # runs, which the gate must catch with --repeat while still seeing byte-identical pairs.
        for parent in target.parents:
            if parent.name.startswith("run"):
                body = body + "  // %s\\n" % parent.name
                break
    target.write_text(body, encoding="utf-8")
elif "--write" in args:
    out = Path(args[args.index("--out") + 1])
    out.mkdir(parents=True, exist_ok=True)
    (out / "Counter.g.hpp").write_text("// generated\\n", encoding="utf-8")
    if not (variant == "cpp_drift" and side == "wasm"):
        (out / "Counter.hpp").write_text("// header\\n", encoding="utf-8")
sys.exit(0)
"""


class WasmIrParityTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name) / "repo"
        (self.root / "samples" / "parity-demo").mkdir(parents=True)
        (self.root / SAMPLE_REL).write_text("export function Counter() {}\n", encoding="utf-8")
        self.bin = Path(self._temp.name) / "bin"
        self.bin.mkdir()

    def tearDown(self) -> None:
        self._temp.cleanup()

    def make_cli(self, name: str) -> Path:
        script = FAKE_CLI.format(
            python=sys.executable, ir_body=IR_BODY, drifted=IR_BODY_DRIFTED
        )
        body = self.bin / (name + ".py")
        body.write_text(script, encoding="utf-8")
        if os.name == "nt":
            # Windows has no shebang support, so the runnable fake is a .bat shim around the
            # interpreter. Both sides go through it, which keeps the fixtures platform-symmetric.
            tool = self.bin / (name + ".bat")
            tool.write_text(
                '@echo off\r\n"{}" "%~dp0{}" %*\r\n'.format(sys.executable, body.name),
                encoding="ascii",
            )
        else:
            tool = self.bin / name
            body.write_text(script, encoding="utf-8")
            body.chmod(body.stat().st_mode | stat.S_IXUSR)
            body.rename(tool)
        return tool

    def run_gate(self, *extra: str, env: dict = None) -> subprocess.CompletedProcess:
        native = self.make_cli("native")
        wasm = self.make_cli("wasm")
        command = [
            sys.executable,
            "-B",
            str(CHECK),
            "--native",
            str(native),
            "--wasm",
            str(wasm),
            "--repo-root",
            str(self.root),
            "--work-dir",
            str(self.root / "build" / "wasm-parity"),
        ]
        command += list(extra)
        merged = dict(os.environ)
        merged.update(env or {})
        return subprocess.run(
            command,
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            encoding="utf-8",
            env=merged,
        )

    def run_pair(self, variant: str, *extra: str) -> subprocess.CompletedProcess:
        """Run the gate with the native/Wasm side split the fake CLI understands."""
        native = self.make_cli("native")
        wasm = self.make_cli("wasm")
        command = [
            sys.executable,
            "-B",
            str(CHECK),
            "--native",
            str(native),
            "--wasm",
            str(wasm),
            "--repo-root",
            str(self.root),
            "--work-dir",
            str(self.root / "build" / "wasm-parity"),
            *extra,
        ]
        merged = dict(os.environ)
        merged["FAKE_SIDE"] = "wasm"
        merged["FAKE_VARIANT"] = variant
        return subprocess.run(
            command,
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            encoding="utf-8",
            env=merged,
        )

    def test_identical_output_passes(self) -> None:
        result = self.run_pair("same")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("BYTE_IDENTICAL ir", result.stdout)
        self.assertIn("PARITY OK", result.stdout)

    def test_ir_mismatch_fails_with_a_diff(self) -> None:
        result = self.run_pair("drift")
        self.assertEqual(result.returncode, 1)
        self.assertIn("MISMATCH ir", result.stdout)
        self.assertIn("CounterX", result.stdout)
        self.assertIn("FAILED: native and Wasm output differ", result.stdout)

    def test_generated_cpp_file_set_mismatch_fails(self) -> None:
        result = self.run_pair("cpp_drift")
        self.assertEqual(result.returncode, 1)
        self.assertIn("MISMATCH", result.stdout)
        self.assertIn("Counter.hpp", result.stdout)

    def test_ir_only_skips_the_cpp_comparison(self) -> None:
        result = self.run_pair("cpp_drift", "--ir-only")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("skipped (--ir-only)", result.stdout)

    def test_repeat_detects_nondeterminism(self) -> None:
        result = self.run_pair("nondeterministic", "--repeat", "2")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not deterministic", result.stdout)

    def test_missing_cli_is_setup_error(self) -> None:
        command = [
            sys.executable,
            "-B",
            str(CHECK),
            "--native",
            str(self.bin / "absent"),
            "--wasm",
            str(self.bin / "absent"),
            "--repo-root",
            str(self.root),
        ]
        result = subprocess.run(
            command, cwd=str(REPO_ROOT), capture_output=True, text=True, encoding="utf-8"
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("SETUP ERROR", result.stderr)


if __name__ == "__main__":
    unittest.main()
