#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-tests for scripts/check_source_size.py (the 500-line ratchet, issue #327).

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

Each test builds a throwaway repository tree in a temporary directory and mutates one thing, so the
assertions cover one violation class at a time. The gate is a ratchet rather than a cliff, and the
cases below are exactly the five behaviours that distinction buys:

* a **new** over-limit file is an error (the gate is not a no-op),
* an over-limit file that **grows** past its baseline is an error (the ratchet only turns one way),
* an over-limit file **at or below** its baseline is a warning (grandfathered, still visible),
* a **stale** baseline entry (file split under the limit) is a warning telling you to remove it,
* a baseline entry naming a **missing** file is an error (the baseline must not rot),
plus the exemptions that make the gate honest: generated goldens, PoC targets and vendored sources
are outside the limit.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
GATE = SCRIPTS_DIR / "check_source_size.py"


def write_lines(path: Path, count: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join("// line {}".format(i) for i in range(count)) + "\n", encoding="utf-8")


class SourceSizeGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.repo = Path(self._temp.name)
        (self.repo / "scripts").mkdir()
        shutil.copy2(GATE, self.repo / "scripts" / GATE.name)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def run_gate(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(self.repo / "scripts" / GATE.name), *args],
            cwd=str(self.repo),
            capture_output=True,
            text=True,
            encoding="utf-8",
        )

    def test_small_files_pass(self) -> None:
        write_lines(self.repo / "src" / "small.cpp", 100)
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 error(s)", result.stdout)

    def test_new_over_limit_file_fails(self) -> None:
        write_lines(self.repo / "src" / "big.cpp", 501)
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ERROR src/big.cpp", result.stdout)
        self.assertIn("exceeds the 500 hard limit", result.stdout)

    def test_growth_past_the_baseline_fails(self) -> None:
        write_lines(self.repo / "src" / "big.cpp", 520)
        self.assertEqual(self.run_gate("--update").returncode, 0)
        write_lines(self.repo / "src" / "big.cpp", 521)
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("exceeds its 520 line baseline", result.stdout)

    def test_grandfathered_file_is_a_warning(self) -> None:
        write_lines(self.repo / "src" / "big.cpp", 520)
        self.assertEqual(self.run_gate("--update").returncode, 0)
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARN src/big.cpp", result.stdout)
        self.assertIn("grandfathered at 520", result.stdout)

    def test_shrinking_to_the_baseline_passes(self) -> None:
        write_lines(self.repo / "src" / "big.cpp", 520)
        self.assertEqual(self.run_gate("--update").returncode, 0)
        # A file may shrink without touching the baseline; the ratchet only forbids growth.
        write_lines(self.repo / "src" / "big.cpp", 510)
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stale_baseline_entry_warns(self) -> None:
        write_lines(self.repo / "src" / "big.cpp", 520)
        self.assertEqual(self.run_gate("--update").returncode, 0)
        write_lines(self.repo / "src" / "big.cpp", 400)
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("baseline entry is stale", result.stdout)

    def test_missing_baselined_file_fails(self) -> None:
        write_lines(self.repo / "src" / "big.cpp", 520)
        self.assertEqual(self.run_gate("--update").returncode, 0)
        (self.repo / "src" / "big.cpp").unlink()
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("names a file that does not exist", result.stdout)

    def test_update_refuses_to_raise_a_baseline(self) -> None:
        write_lines(self.repo / "src" / "big.cpp", 520)
        self.assertEqual(self.run_gate("--update").returncode, 0)
        write_lines(self.repo / "src" / "big.cpp", 540)
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("grew from 520 to 540", result.stderr)
        # The refused update must not have written the higher number.
        baseline = (self.repo / "scripts" / "source-size-baseline.txt").read_text(encoding="utf-8")
        self.assertIn("src/big.cpp 520", baseline)

    def test_exemptions_are_not_counted(self) -> None:
        write_lines(self.repo / "src" / "big.cpp", 501)
        for exempt in (
            "tests/golden/codegen/Sample.g.hpp",
            "tools/poc/batched2d/main.cpp",
            "third_party/stb/stb_image.h",
            "build/dev/generated.cpp",
        ):
            write_lines(self.repo / exempt, 900)
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        # Only the real source file is a violation; the four exempt ones are silent.
        self.assertEqual(result.stdout.count("ERROR"), 1, result.stdout)
        self.assertIn("ERROR src/big.cpp", result.stdout)
        for exempt in ("golden", "poc", "third_party", "build/"):
            self.assertNotIn(exempt, result.stdout)


if __name__ == "__main__":
    unittest.main()
