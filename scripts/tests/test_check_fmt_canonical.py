#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for scripts/check_fmt_canonical.py: the baseline ratchet, with a fake canonical predicate."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_fmt_canonical as ratchet  # noqa: E402


class RatchetTests(unittest.TestCase):
    def test_new_file_must_be_canonical(self) -> None:
        problems = ratchet.check(["samples/a.tsx"], set(), lambda name: False)
        self.assertEqual(len(problems), 1)
        self.assertIn("not canonical", problems[0])

    def test_baseline_file_may_stay_non_canonical(self) -> None:
        self.assertEqual(ratchet.check(["samples/a.tsx"], {"samples/a.tsx"}, lambda name: False), [])

    def test_baseline_shrinks_when_a_file_is_formatted(self) -> None:
        problems = ratchet.check(["samples/a.tsx"], {"samples/a.tsx"}, lambda name: True)
        self.assertIn("remove it from the baseline", problems[0])

    def test_stale_baseline_entry_is_reported(self) -> None:
        problems = ratchet.check([], {"samples/gone.tsx"}, lambda name: True)
        self.assertIn("not in the tree", problems[0])

    def test_scope_covers_samples_and_packages_only(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for rel in ("samples/x/A.tsx", "samples/x/tessera.json", "samples/x/node_modules/B.tsx",
                        "packages/tessera-ui/c.css", "tools/wiki/d.tsx", "samples/x/readme.md"):
                path = root / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            self.assertEqual(
                ratchet.in_scope(root),
                ["packages/tessera-ui/c.css", "samples/x/A.tsx", "samples/x/tessera.json"],
            )


if __name__ == "__main__":
    unittest.main()
