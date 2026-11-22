#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-tests for scripts/check_enum_size.py (the enum base-type gate, issue #734).

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

Each test builds a throwaway repository tree in a temporary directory and mutates one thing, so the
assertions cover one violation class at a time:

* an untyped `enum class` in `include/` or `src/` is an error (the gate is not a no-op),
* a declaration that names a base type passes,
* `enum struct` is the same construct and is checked too,
* a brace on the next line is still recognised (the tree uses both styles),
* an allowlisted declaration passes, and a **stale** allowlist entry is an error (the exception
  list must not rot),
* `tests/` and `tools/` are out of scope, so a declaration there does not fail the gate.
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
GATE = SCRIPTS_DIR / "check_enum_size.py"


class EnumSizeGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.repo = Path(self._temp.name)
        (self.repo / "scripts").mkdir()
        shutil.copy2(GATE, self.repo / "scripts" / GATE.name)
        (self.repo / "include").mkdir()
        (self.repo / "src").mkdir()

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_gate(self, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(self.repo / "scripts" / GATE.name), *extra],
            cwd=self.repo,
            capture_output=True,
            text=True,
        )

    def test_untyped_enum_is_an_error(self) -> None:
        self.write("include/a.hpp", "enum class Colour { Red, Green };\n")
        result = self.run_gate()
        self.assertEqual(1, result.returncode, result.stdout)
        self.assertIn("enum class Colour has no base type", result.stdout)

    def test_typed_enum_passes(self) -> None:
        self.write("include/a.hpp", "enum class Colour : std::uint8_t { Red, Green };\n")
        result = self.run_gate()
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertIn("0 error(s)", result.stdout)

    def test_enum_struct_is_checked(self) -> None:
        self.write("src/a.cpp", "enum struct Mode { A, B };\n")
        result = self.run_gate()
        self.assertEqual(1, result.returncode, result.stdout)
        self.assertIn("enum class Mode has no base type", result.stdout)

    def test_brace_on_the_next_line_is_recognised(self) -> None:
        self.write("include/a.hpp", "enum class Colour\n{\n    Red,\n};\n")
        result = self.run_gate()
        self.assertEqual(1, result.returncode, result.stdout)
        self.assertIn("enum class Colour has no base type", result.stdout)

    def test_allowlisted_declaration_passes(self) -> None:
        self.write("include/a.hpp", "enum class Colour { Red, Green };\n")
        self.write("scripts/enum-size-allowlist.txt", "include/a.hpp:Colour  ABI contract.\n")
        result = self.run_gate()
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertIn("1 allowlisted", result.stdout)

    def test_stale_allowlist_entry_is_an_error(self) -> None:
        self.write("include/a.hpp", "enum class Colour : std::uint8_t { Red };\n")
        self.write("scripts/enum-size-allowlist.txt", "include/a.hpp:Colour  Fixed already.\n")
        result = self.run_gate()
        self.assertEqual(1, result.returncode, result.stdout)
        self.assertIn("matches no untyped enum class", result.stdout)

    def test_tests_and_tools_are_out_of_scope(self) -> None:
        self.write("tests/a.cpp", "enum class Colour { Red };\n")
        self.write("tools/a.cpp", "enum class Mode { A };\n")
        result = self.run_gate()
        self.assertEqual(0, result.returncode, result.stdout)


if __name__ == "__main__":
    unittest.main()
