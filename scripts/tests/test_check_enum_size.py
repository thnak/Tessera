#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-tests for scripts/check_enum_size.py (the untyped `enum class` ratchet, issue #734).

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

Each test builds a throwaway repository tree in a temporary directory and mutates one thing, so the
assertions cover one violation class at a time:

* a **new** untyped declaration is an error (the gate is not a no-op),
* a typed declaration is not (the fix the gate asks for is accepted),
* a baselined file that **grows** past its count is an error (the ratchet only turns one way),
* a baselined file that is now fully typed is a warning telling you to remove the line,
* a baseline entry naming a **missing** file is an error (the baseline must not rot),
* the exemptions that make the gate honest: `third_party/`, `tests/` and build trees are outside it,
* a declaration inside a **comment** is not a declaration (the scanner blanks comments first).
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


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class EnumSizeGateTests(unittest.TestCase):
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

    def test_typed_enum_passes(self) -> None:
        write(
            self.repo / "include" / "ok.hpp",
            "#include <cstdint>\nenum class Good : std::uint8_t { A, B };\n",
        )
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 error(s)", result.stdout)

    def test_new_untyped_enum_fails(self) -> None:
        write(self.repo / "include" / "bad.hpp", "enum class Bad { A, B };\n")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ERROR include/bad.hpp:1 Bad", result.stdout)
        self.assertIn("std::uint8_t", result.stdout)

    def test_enum_in_a_comment_is_not_counted(self) -> None:
        # The scanner blanks comments before matching, so a documented example is not a violation.
        write(
            self.repo / "include" / "doc.hpp",
            "// enum class Example { A };\n/* enum class Other { B }; */\n"
            "enum class Real : std::uint8_t { A };\n",
        )
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_growth_past_the_baseline_fails(self) -> None:
        write(self.repo / "include" / "two.hpp", "enum class A { X };\n")
        self.assertEqual(self.run_gate("--update").returncode, 0)
        write(self.repo / "include" / "two.hpp", "enum class A { X };\nenum class B { Y };\n")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("exceeds this file's 1 declaration baseline", result.stdout)

    def test_grandfathered_file_is_a_warning(self) -> None:
        write(self.repo / "include" / "one.hpp", "enum class A { X };\n")
        self.assertEqual(self.run_gate("--update").returncode, 0)
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARN include/one.hpp", result.stdout)

    def test_typing_a_baselined_file_warns_to_remove_the_line(self) -> None:
        write(self.repo / "include" / "one.hpp", "enum class A { X };\n")
        self.assertEqual(self.run_gate("--update").returncode, 0)
        write(self.repo / "include" / "one.hpp", "enum class A : std::uint8_t { X };\n")
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("baseline entry is stale", result.stdout)

    def test_missing_baselined_file_fails(self) -> None:
        write(self.repo / "include" / "one.hpp", "enum class A { X };\n")
        self.assertEqual(self.run_gate("--update").returncode, 0)
        (self.repo / "include" / "one.hpp").unlink()
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("names a file that does not exist", result.stdout)

    def test_update_refuses_to_raise_a_baseline(self) -> None:
        write(self.repo / "include" / "one.hpp", "enum class A { X };\n")
        self.assertEqual(self.run_gate("--update").returncode, 0)
        write(self.repo / "include" / "one.hpp", "enum class A { X };\nenum class B { Y };\n")
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("type them instead of raising the baseline", result.stderr)

    def test_exemptions_are_not_counted(self) -> None:
        write(self.repo / "include" / "bad.hpp", "enum class Bad { A };\n")
        for exempt in (
            "third_party/lib/x.hpp",
            "tests/unit/local.hpp",
            "build/dev/generated.hpp",
        ):
            write(self.repo / exempt, "enum class Exempt { A };\n")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("ERROR"), 1, result.stdout)
        self.assertIn("ERROR include/bad.hpp", result.stdout)
        for exempt in ("third_party", "tests/", "build/"):
            self.assertNotIn(exempt, result.stdout)


class TrackedFilesEnumGateTests(unittest.TestCase):
    """The file list comes from `git ls-files`, so untracked trees are invisible (issue #733 item 6)."""

    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.repo = Path(self._temp.name)
        (self.repo / "scripts").mkdir()
        shutil.copy2(GATE, self.repo / "scripts" / GATE.name)
        self._git("init", "-q")
        self._git("config", "user.email", "gate@example.invalid")
        self._git("config", "user.name", "gate")

    def tearDown(self) -> None:
        self._temp.cleanup()

    def _git(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["git", *args], cwd=str(self.repo), capture_output=True, text=True, encoding="utf-8"
        )

    def run_gate(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(self.repo / "scripts" / GATE.name), *args],
            cwd=str(self.repo),
            capture_output=True,
            text=True,
            encoding="utf-8",
        )

    def test_untracked_tree_is_not_counted(self) -> None:
        write(self.repo / "include" / "ok.hpp", "enum class Good : std::uint8_t { A };\n")
        write(self.repo / ".claude" / "worktrees" / "lane" / "bad.hpp", "enum class Bad { A };\n")
        self.assertEqual(self._git("add", "include").returncode, 0)
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn(".claude", result.stdout)

    def test_tracked_untyped_enum_still_fails(self) -> None:
        write(self.repo / "include" / "bad.hpp", "enum class Bad { A };\n")
        self.assertEqual(self._git("add", "include").returncode, 0)
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ERROR include/bad.hpp", result.stdout)


if __name__ == "__main__":
    unittest.main()
