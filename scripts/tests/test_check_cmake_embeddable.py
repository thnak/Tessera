#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-tests for scripts/check_cmake_embeddable.py (the embeddable-build gate).

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

Each test builds a throwaway tree and checks one rule:

* `CMAKE_SOURCE_DIR` or `CMAKE_BINARY_DIR` in a CMakeLists.txt or *.cmake is an error,
* `PROJECT_SOURCE_DIR` and `CMAKE_CURRENT_*` are fine,
* a comment that names the variable is not code,
* the "am I top level" comparison is the one accepted use,
* `third_party/` and build output are out of scope.
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
GATE = SCRIPTS_DIR / "check_cmake_embeddable.py"


class CmakeEmbeddableGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.repo = Path(self._temp.name)
        (self.repo / "scripts").mkdir()
        shutil.copy2(GATE, self.repo / "scripts" / GATE.name)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_gate(self) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(self.repo / "scripts" / GATE.name)],
            capture_output=True,
            text=True,
            encoding="utf-8",
        )

    def test_source_dir_is_an_error(self) -> None:
        self.write("src/CMakeLists.txt", "add_executable(a ${CMAKE_SOURCE_DIR}/tools/main.cpp)\n")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("src/CMakeLists.txt:1", result.stdout)

    def test_binary_dir_in_a_cmake_module_is_an_error(self) -> None:
        self.write("cmake/Thing.cmake", 'set(out "${CMAKE_BINARY_DIR}/gen")\n')
        self.assertEqual(self.run_gate().returncode, 1)

    def test_project_and_current_dirs_pass(self) -> None:
        self.write(
            "src/CMakeLists.txt",
            "add_executable(a ${PROJECT_SOURCE_DIR}/x.cpp ${CMAKE_CURRENT_SOURCE_DIR}/y.cpp\n"
            "  ${PROJECT_BINARY_DIR}/z.cpp)\n",
        )
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_a_comment_is_not_code(self) -> None:
        self.write("CMakeLists.txt", "# not ${CMAKE_SOURCE_DIR} but PROJECT_SOURCE_DIR\nproject(x)\n")
        self.assertEqual(self.run_gate().returncode, 0)

    def test_the_top_level_comparison_is_accepted(self) -> None:
        self.write(
            "tools/CMakeLists.txt",
            "if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)\nendif()\n",
        )
        self.assertEqual(self.run_gate().returncode, 0)

    def test_the_comparison_does_not_excuse_a_second_use(self) -> None:
        self.write(
            "tools/CMakeLists.txt",
            "if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND EXISTS ${CMAKE_SOURCE_DIR}/x)\n",
        )
        self.assertEqual(self.run_gate().returncode, 1)

    def test_third_party_and_build_output_are_out_of_scope(self) -> None:
        self.write("third_party/lib/CMakeLists.txt", "set(a ${CMAKE_SOURCE_DIR})\n")
        self.write("build/dev/CMakeFiles/x.cmake", "set(a ${CMAKE_BINARY_DIR})\n")
        self.assertEqual(self.run_gate().returncode, 0)


if __name__ == "__main__":
    unittest.main()
