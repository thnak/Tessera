#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-tests for scripts/ci/gen_sample_manifest.py (the P5 sample gallery manifest, issue #169).

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

The generator is the thing that keeps "adding a sample does not mean editing JavaScript" true, so
these cases are about the properties that promise rests on:

* the manifest is derived from `samples/**/*.tsx` alone -- adding a file adds an entry, removing one
  removes it, and nothing else has to be touched;
* the same input always produces the same bytes, or every run would show a spurious diff;
* `--check` fails on a stale manifest and names the samples that changed, because "stale" without the
  diff makes a reviewer work it out by hand;
* the entry component is the first *capitalized* export (a lower-case helper is not a screen);
* the manifest carries a path and a component name and **no source text**, so the TSX stays the single
  source of truth;
* an empty or unnamed sample is a setup error, not a silently smaller gallery.

Each test runs the real script against a throwaway repository, so the assertions are about what a user
sees (the exit code, the message, the bytes written) rather than about internals.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
GENERATOR = REPO_ROOT / "scripts" / "ci" / "gen_sample_manifest.py"

COUNTER = """// A sample screen.
export function Counter(): Node {
  return <View />;
}
"""

# A helper *before* the component: the generator must skip it and take the capitalized export.
WITH_HELPER = """export const format = (value: number): string => String(value);

export function Screen(): Node {
  return <View />;
}
"""


class SampleManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        (self.root / "scripts" / "ci").mkdir(parents=True)
        (self.root / "designer" / "web").mkdir(parents=True)
        self.manifest = self.root / "designer" / "web" / "samples.json"
        self.samples = self.root / "samples"
        self.samples.mkdir()

    def tearDown(self) -> None:
        self._temp.cleanup()

    def add_sample(self, relative: str, text: str = COUNTER) -> None:
        path = self.samples / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_generator(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(GENERATOR),
                "--repo-root",
                str(self.root),
                "--manifest",
                str(self.manifest),
                *args,
            ],
            cwd=str(self.root),
            capture_output=True,
            text=True,
            encoding="utf-8",
        )

    def read_manifest(self) -> dict:
        return json.loads(self.manifest.read_text(encoding="utf-8"))

    # --- generation ----------------------------------------------------------------------------

    def test_enumerates_every_tsx_sorted_by_path(self) -> None:
        self.add_sample("zeta/Zeta.tsx")
        self.add_sample("alpha/Alpha.tsx")
        result = self.run_generator()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        paths = [entry["path"] for entry in self.read_manifest()["samples"]]
        self.assertEqual(paths, ["samples/alpha/Alpha.tsx", "samples/zeta/Zeta.tsx"])

    def test_entry_component_skips_a_lower_case_helper(self) -> None:
        self.add_sample("demo/Screen.tsx", WITH_HELPER)
        self.run_generator()
        self.assertEqual(self.read_manifest()["samples"][0]["component"], "Screen")

    def test_manifest_carries_no_source_text(self) -> None:
        self.add_sample("demo/Counter.tsx")
        self.run_generator()
        text = self.manifest.read_text(encoding="utf-8")
        self.assertNotIn("useState", text)
        self.assertNotIn("Node", text)
        entry = self.read_manifest()["samples"][0]
        self.assertEqual(sorted(entry), ["component", "files", "path"], "a path, a name and companions")

    def test_output_is_byte_identical_across_runs(self) -> None:
        self.add_sample("demo/Counter.tsx")
        self.add_sample("other/Other.tsx")
        self.run_generator()
        first = self.manifest.read_bytes()
        self.run_generator()
        self.assertEqual(self.manifest.read_bytes(), first, "a rerun must not churn the diff")

    def test_nested_directories_are_found(self) -> None:
        self.add_sample("deep/nested/dir/Thing.tsx")
        self.run_generator()
        self.assertEqual(
            self.read_manifest()["samples"][0]["path"], "samples/deep/nested/dir/Thing.tsx"
        )

    # --- companion files (a sample is a small project) ---------------------------------------------

    def add_library(self, name: str, text: str) -> None:
        path = self.root / "packages" / "tessera-ui" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def files_of(self, sample: str) -> list:
        entries = {entry["path"]: entry for entry in self.read_manifest()["samples"]}
        return entries[sample]["files"]

    def test_a_sibling_import_is_a_companion_at_its_own_path(self) -> None:
        self.add_sample("demo/Page.tsx", 'import { Row } from "./Row";\n' + COUNTER)
        self.add_sample("demo/Row.tsx")
        self.assertEqual(self.run_generator().returncode, 0)
        self.assertEqual(
            self.files_of("samples/demo/Page.tsx"),
            [{"as": "samples/demo/Row.tsx", "from": "samples/demo/Row.tsx"}],
        )

    def test_a_library_import_comes_from_the_component_library(self) -> None:
        self.add_library("Button.tsx", COUNTER)
        self.add_sample("demo/Page.tsx", 'import { Button } from "./Button";\n' + COUNTER)
        self.assertEqual(self.run_generator().returncode, 0)
        self.assertEqual(
            self.files_of("samples/demo/Page.tsx"),
            [{"as": "samples/demo/Button.tsx", "from": "packages/tessera-ui/Button.tsx"}],
        )

    def test_a_sibling_wins_over_the_library(self) -> None:
        self.add_library("Button.tsx", COUNTER)
        self.add_sample("demo/Button.tsx")
        self.add_sample("demo/Page.tsx", 'import { Button } from "./Button";\n' + COUNTER)
        self.run_generator()
        self.assertEqual(self.files_of("samples/demo/Page.tsx")[0]["from"], "samples/demo/Button.tsx")

    def test_imports_are_followed_recursively_and_stylesheets_are_included(self) -> None:
        self.add_library("Button.tsx", 'import "./button.css";\n' + COUNTER)
        self.add_library("button.css", ".a {}\n")
        self.add_sample(
            "demo/Page.tsx",
            'import { useState } from "tessera";\nimport { Button } from "./Button";\n'
            'import styles from "./page.module.css";\n' + COUNTER,
        )
        (self.samples / "demo" / "page.module.css").write_text(".b {}\n", encoding="utf-8")
        self.assertEqual(self.run_generator().returncode, 0)
        self.assertEqual(
            [file["as"] for file in self.files_of("samples/demo/Page.tsx")],
            ["samples/demo/Button.tsx", "samples/demo/button.css", "samples/demo/page.module.css"],
        )

    def test_commented_out_imports_and_bare_specifiers_are_ignored(self) -> None:
        self.add_sample(
            "demo/Page.tsx",
            '// import { Gone } from "./Gone";\nimport { useState } from "tessera";\n' + COUNTER,
        )
        self.assertEqual(self.run_generator().returncode, 0)
        self.assertEqual(self.files_of("samples/demo/Page.tsx"), [])

    def test_an_unresolved_import_fails_the_generator_and_the_check(self) -> None:
        self.add_sample("demo/Page.tsx", 'import { Nope } from "./Nope";\n' + COUNTER)
        for args in ((), ("--check",)):
            result = self.run_generator(*args)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("samples/demo/Page.tsx: import `./Nope` resolves to neither", result.stderr)
        self.assertFalse(self.manifest.exists(), "nothing is written for a sample that cannot resolve")

    def test_an_import_leaving_the_samples_directory_is_refused(self) -> None:
        self.add_sample("demo/Page.tsx", 'import { X } from "../other/X";\n' + COUNTER)
        self.add_sample("other/X.tsx")
        result = self.run_generator()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("leaves the sample's directory", result.stderr)

    # --- drift ---------------------------------------------------------------------------------

    def test_check_passes_immediately_after_generating(self) -> None:
        self.add_sample("demo/Counter.tsx")
        self.run_generator()
        result = self.run_generator("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("up to date", result.stdout)

    def test_check_fails_and_names_a_new_sample(self) -> None:
        self.add_sample("demo/Counter.tsx")
        self.run_generator()
        self.add_sample("demo/Added.tsx")
        result = self.run_generator("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("stale", result.stderr)
        self.assertIn("missing: samples/demo/Added.tsx", result.stderr)

    def test_check_fails_and_names_a_removed_sample(self) -> None:
        self.add_sample("demo/Counter.tsx")
        self.add_sample("demo/Gone.tsx")
        self.run_generator()
        (self.samples / "demo" / "Gone.tsx").unlink()
        result = self.run_generator("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("removed: samples/demo/Gone.tsx", result.stderr)

    def test_check_fails_when_the_manifest_is_missing(self) -> None:
        self.add_sample("demo/Counter.tsx")
        result = self.run_generator("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("is missing", result.stderr)
        self.assertFalse(self.manifest.exists(), "--check must not write")

    def test_check_fails_when_only_the_component_changed(self) -> None:
        self.add_sample("demo/Counter.tsx")
        self.run_generator()
        # A rename that keeps the path: the diff has no added or removed path, so the message has to
        # say that rather than printing two empty lists.
        self.add_sample("demo/Counter.tsx", COUNTER.replace("Counter", "Renamed"))
        result = self.run_generator("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("companion files or format changed", result.stderr)

    def test_check_reports_a_hand_mangled_manifest(self) -> None:
        self.add_sample("demo/Counter.tsx")
        self.run_generator()
        self.manifest.write_text("{not json", encoding="utf-8")
        result = self.run_generator("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not a schema-2 manifest", result.stderr)

    # --- failure modes -------------------------------------------------------------------------

    def test_an_empty_samples_directory_is_a_setup_error(self) -> None:
        result = self.run_generator()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("no .tsx files", result.stderr)

    def test_a_sample_without_a_component_is_a_setup_error(self) -> None:
        self.add_sample("demo/Helper.tsx", "export const helper = 1;\n")
        result = self.run_generator()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("no capitalized component", result.stderr)

    def test_the_error_names_the_file(self) -> None:
        self.add_sample("demo/Good.tsx")
        self.add_sample("demo/Bad.tsx", "export const nothing = 1;\n")
        result = self.run_generator()
        self.assertIn("samples/demo/Bad.tsx", result.stderr)
        # Nothing is written when a sample cannot be described: a partial gallery is worse than none.
        self.assertFalse(self.manifest.exists())


if __name__ == "__main__":
    unittest.main()
