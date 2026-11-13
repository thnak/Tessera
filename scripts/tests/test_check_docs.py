#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture-based self-tests for scripts/check_docs.py.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

Each test copies the clean fixture tree to a temporary directory and mutates one thing, so the
assertions cover one violation class at a time (and the clean tree itself must stay green).
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
CHECK = SCRIPTS_DIR / "check_docs.py"
FIXTURE = TESTS_DIR / "fixtures" / "docs" / "clean"


class DocsGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name) / "repo"
        shutil.copytree(FIXTURE, self.root)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def run_check(self, *extra: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(CHECK), "--repo-root", str(self.root), *extra],
            capture_output=True,
            text=True, encoding="utf-8", errors="replace",
            cwd=str(cwd or self.root),
        )

    def append(self, relative: str, text: str) -> None:
        path = self.root / relative
        with path.open("a", encoding="utf-8") as handle:
            handle.write(text)

    def test_clean_tree_passes(self) -> None:
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 error(s), 0 warning(s)", result.stdout)

    def test_broken_relative_link_is_an_error(self) -> None:
        self.append("docs/guide.md", "\n[Missing](missing.md)\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ERROR docs/guide.md:", result.stdout)
        self.assertIn("broken relative link 'missing.md'", result.stdout)

    def test_bad_fragment_is_an_error(self) -> None:
        self.append("docs/guide.md", "\n[Bad anchor](folder/part.md#nope)\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no heading with anchor '#nope'", result.stdout)

    def test_anchor_with_punctuation_matches_github_slug(self) -> None:
        self.append("docs/guide.md", "\n[Real anchor](README.md#fixture-docs-map)\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_orphan_doc_is_an_error(self) -> None:
        (self.root / "docs" / "orphan.md").write_text("# Orphan\n", encoding="utf-8")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ERROR docs/orphan.md:1 not referenced from docs/README.md", result.stdout)

    def test_unclosed_fence_is_an_error(self) -> None:
        self.append("docs/guide.md", "\n```bash\necho 'never closed'\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("unclosed code fence", result.stdout)

    def test_nested_fence_is_balanced(self) -> None:
        self.append(
            "docs/guide.md",
            "\n````markdown\n"
            "```bash\n"
            "echo nested fence\n"
            "```\n"
            "````\n",
        )
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_oversized_doc_is_an_error(self) -> None:
        big = "# Big\n\n" + ("fixture line\n" * 610)
        (self.root / "docs" / "big.md").write_text(big, encoding="utf-8")
        self.append("docs/README.md", "\n- [`big.md`](big.md)\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("exceeds the 600 hard limit", result.stdout)

    def test_soft_size_limit_is_a_warning_only(self) -> None:
        medium = "# Medium\n\n" + ("fixture line\n" * 450)
        (self.root / "docs" / "medium.md").write_text(medium, encoding="utf-8")
        self.append("docs/README.md", "\n- [`medium.md`](medium.md)\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARN docs/medium.md:1", result.stdout)
        self.assertIn("exceeds the 400 line target", result.stdout)

    def test_missing_adr_index_row_is_an_error(self) -> None:
        (self.root / "decisions" / "ADR-002-second.md").write_text(
            "# ADR-002: Second fixture\n\n- Status: Accepted\n", encoding="utf-8"
        )
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ADR-002-second.md:1 ADR is not listed in decisions/README.md", result.stdout)

    def test_adr_missing_heading_is_an_error(self) -> None:
        (self.root / "decisions" / "ADR-003-bad.md").write_text(
            "No heading here\n\n- Status: Accepted\n", encoding="utf-8"
        )
        self.append("decisions/README.md", "\n| [ADR-003](ADR-003-bad.md) | Bad | Accepted |\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("must start with an '# ' heading", result.stdout)

    def test_adr_missing_status_is_an_error(self) -> None:
        (self.root / "decisions" / "ADR-004-no-status.md").write_text(
            "# ADR-004: No status\n\n## Context\n", encoding="utf-8"
        )
        self.append(
            "decisions/README.md", "\n| [ADR-004](ADR-004-no-status.md) | No status | Accepted |\n"
        )
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("missing a '- Status:' line", result.stdout)

    def test_warnings_only_exit_zero(self) -> None:
        self.append(
            "docs/guide.md",
            "\n[Directory](folder/)\n[Outside](../../outside.md)\n",
        )
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARN docs/guide.md:", result.stdout)
        self.assertIn("points to a directory", result.stdout)
        self.assertIn("points outside the repository", result.stdout)

    def test_invalid_utf8_is_an_error(self) -> None:
        (self.root / "docs" / "encoding.md").write_bytes(b"# Encoding\n\nBad \xa7 byte\n")
        self.append("docs/README.md", "\n- [`encoding.md`](encoding.md)\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("file is not valid UTF-8", result.stdout)

    def test_json_report_is_machine_readable(self) -> None:
        result = self.run_check("--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "docs")
        self.assertEqual(payload["errors"], [])
        self.assertGreater(payload["files_checked"], 0)

    def test_cwd_independent(self) -> None:
        result = self.run_check(cwd=Path(self._temp.name))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    # --- ADR-049: an RHI header change needs version.hpp and the RHI contract doc ---------------

    def init_git(self) -> None:
        """The RHI gate compares against a git base, so these cases run in a real repository."""
        for args in (
            ("init", "-q"),
            ("config", "user.email", "gate@example.invalid"),
            ("config", "user.name", "docs gate"),
            ("config", "commit.gpgsign", "false"),
            ("add", "-A"),
            ("commit", "-q", "-m", "fixture"),
        ):
            subprocess.run(["git", "-C", str(self.root), *args], check=True,
                           capture_output=True)

    def commit_all(self, message: str = "change") -> None:
        for args in (("add", "-A"), ("commit", "-q", "-m", message)):
            subprocess.run(["git", "-C", str(self.root), *args], check=True,
                           capture_output=True)

    def write_rhi_header(self) -> None:
        header = self.root / "include" / "tessera" / "rendering" / "rhi" / "types.hpp"
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text("#pragma once\n\nstruct FixtureTypes {};\n", encoding="utf-8")

    def test_committed_rhi_change_without_version_and_docs_is_an_error(self) -> None:
        self.init_git()
        self.write_rhi_header()
        self.commit_all()
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(
            "ERROR include/tessera/rendering/rhi/version.hpp:1 RHI headers changed", result.stdout
        )
        self.assertIn("ERROR docs/design/gpu/rhi.md:1 RHI headers changed", result.stdout)

    def test_rhi_change_with_version_but_without_docs_is_an_error(self) -> None:
        self.init_git()
        self.write_rhi_header()
        version = self.root / "include" / "tessera" / "rendering" / "rhi" / "version.hpp"
        version.write_text("#pragma once\n", encoding="utf-8")
        self.commit_all()
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("rhi/version.hpp:1 RHI headers changed", result.stdout)
        self.assertIn("ERROR docs/design/gpu/rhi.md:1 RHI headers changed", result.stdout)

    def test_uncommitted_rhi_change_with_version_and_docs_passes(self) -> None:
        self.init_git()
        self.write_rhi_header()
        version = self.root / "include" / "tessera" / "rendering" / "rhi" / "version.hpp"
        version.write_text("#pragma once\n", encoding="utf-8")
        gpu_docs = self.root / "docs" / "design" / "gpu"
        gpu_docs.mkdir(parents=True, exist_ok=True)
        (gpu_docs / "rhi.md").write_text(
            "# RHI contract\n\nVersion 1.0.\n", encoding="utf-8"
        )
        self.append("docs/README.md", "\n- [`design/gpu/rhi.md`](design/gpu/rhi.md) — RHI.\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rhi_change_in_an_unrelated_file_is_ignored(self) -> None:
        self.init_git()
        self.append("docs/guide.md", "\nMore guide text.\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
