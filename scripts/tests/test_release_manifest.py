#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""End-to-end tests for the release-manifest commands (issue #193).

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

Every case runs the real ``gen_release_manifest.py`` / ``check_release_manifest.py`` against a
throwaway repository, so the assertions are about what a user sees -- exit codes, messages, bytes on
disk -- rather than about internals. The properties they protect:

* **Determinism.** Two ``project`` runs produce identical bytes and the second writes nothing; two
  ``--json`` check runs print identical output.
* **The ratchet.** ``record`` refuses a duplicate version, a symbol file for another build, a symbol
  file with no names and a commit that disagrees with the archive; ``project`` refuses to write when
  the data violates a rule. A refusal writes nothing.
* **The gate.** Hand-edited, missing and extra manifests, an off-allowlist URL and a signature with
  no pinned key all fail; a clean tree passes.

Fixtures live in ``release_fixtures.py``; the pure-rule cases are in ``test_release_rules.py``.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import unittest

from release_fixtures import (
    CHANNELS,
    CHECKER,
    COMMIT,
    GENERATOR,
    PLATFORM,
    REPO_ROOT,
    VERSION,
    TempRepositoryCase,
    ledger_with,
    model,
    package_info,
    release_record,
    symbols_text,
    write_archive,
)


class RecordTests(TempRepositoryCase):
    def test_record_hashes_the_real_bytes_and_projects(self) -> None:
        result = self.record()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        artifact = self.repo.read_ledger()["releases"][0]["artifacts"][0]
        archive = self.repo.path("tessera-{}-{}.zip".format(VERSION, PLATFORM))
        self.assertEqual(artifact["sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertEqual(artifact["size"], archive.stat().st_size)
        self.assertEqual(artifact["symbols"]["build"], COMMIT)
        self.assertEqual(artifact["symbols"]["functions"], 3)
        manifest = json.loads(self.repo.manifest().read_text(encoding="utf-8"))
        self.assertEqual(manifest["channel"], "beta")
        self.assertEqual(manifest["version"], VERSION)
        self.assertEqual(manifest["sha256"], artifact["sha256"])
        self.assertEqual(manifest["symbols"]["artifact"], artifact["symbols"]["artifact"])
        checker = self.repo.run(CHECKER)
        self.assertEqual(checker.returncode, 0, checker.stdout + checker.stderr)

    def test_two_project_runs_are_byte_identical(self) -> None:
        self.assertEqual(self.record().returncode, 0)
        path = self.repo.manifest()
        first = path.read_bytes()
        result = self.repo.run(GENERATOR, "project")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("nothing written", result.stdout)
        self.assertEqual(first, path.read_bytes(), "a second project run changed the bytes")

    def test_record_refuses_a_duplicate_version(self) -> None:
        self.assertEqual(self.record().returncode, 0)
        again = self.record()
        self.assertEqual(again.returncode, 1, again.stdout + again.stderr)
        self.assertIn("append-only", again.stderr)

    def test_record_refuses_a_symbol_file_for_another_build(self) -> None:
        archive = write_archive(
            self.repo.path("tessera-{}-{}.zip".format(VERSION, PLATFORM)), package_info()
        )
        symbols = self.repo.path("tessera-{}-{}.symbols.txt".format(VERSION, PLATFORM))
        symbols.write_text(symbols_text(commit="1" * 40), encoding="utf-8")
        result = self.repo.run(
            GENERATOR,
            "record",
            "--archive",
            str(archive),
            "--symbols",
            str(symbols),
            "--channel",
            "beta",
            "--date",
            "2026-11-16",
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not the release commit", result.stderr)

    def test_record_refuses_a_symbol_file_with_no_names(self) -> None:
        archive = write_archive(
            self.repo.path("tessera-{}-{}.zip".format(VERSION, PLATFORM)), package_info()
        )
        symbols = self.repo.path("tessera-{}-{}.symbols.txt".format(VERSION, PLATFORM))
        symbols.write_text(symbols_text(functions=0), encoding="utf-8")
        result = self.repo.run(
            GENERATOR,
            "record",
            "--archive",
            str(archive),
            "--symbols",
            str(symbols),
            "--channel",
            "beta",
            "--date",
            "2026-11-16",
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("no func= rows", result.stderr)

    def test_record_refuses_commit_disagreement(self) -> None:
        result = self.record("--commit", "0" * 40)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("disagrees", result.stderr)

    def test_record_refuses_an_unknown_channel(self) -> None:
        result = self.record("--channel", "canary")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not defined", result.stderr)

    def test_projection_refuses_an_invalid_ledger(self) -> None:
        # A release newer than project(Tessera VERSION) cannot be projected: the tree would ship a
        # version it never bumped to.
        self.repo.write_ledger(ledger_with(release_record("9.9.9")))
        result = self.repo.run(GENERATOR, "project")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ledger.project-version", result.stderr)
        self.assertFalse(
            self.repo.manifest().exists(), "a refused projection must write nothing"
        )


class CheckTests(TempRepositoryCase):
    def seed(self) -> None:
        self.repo.write_ledger(ledger_with(release_record(VERSION)))
        result = self.repo.run(GENERATOR, "project")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_clean_tree_passes(self) -> None:
        self.seed()
        result = self.repo.run(CHECKER)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 error(s)", result.stdout)

    def test_hand_edited_manifest_is_drift(self) -> None:
        self.seed()
        path = self.repo.manifest()
        document = json.loads(path.read_text(encoding="utf-8"))
        document["version"] = "0.0.2"
        path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        result = self.repo.run(CHECKER)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("manifests.drift", result.stdout)
        check = self.repo.run(GENERATOR, "project", "--check")
        self.assertEqual(check.returncode, 1, check.stdout + check.stderr)

    def test_missing_manifest_is_a_projection_error(self) -> None:
        self.seed()
        self.repo.manifest().unlink()
        result = self.repo.run(CHECKER)
        self.assertEqual(result.returncode, 1)
        self.assertIn("manifests.projection", result.stdout)

    def test_extra_manifest_is_a_projection_error(self) -> None:
        self.seed()
        self.repo.path("release", "manifests", "stable-linux-x86_64.json").write_text(
            "{}\n", encoding="utf-8"
        )
        result = self.repo.run(CHECKER)
        self.assertEqual(result.returncode, 1)
        self.assertIn("manifests.projection", result.stdout)

    def test_url_outside_the_allowlist_is_refused(self) -> None:
        self.repo.write_channels(dict(CHANNELS, updateHosts=["updates.example.com"]))
        release = release_record(VERSION)
        release["artifacts"][0]["url"] = "https://evil.example.net/app.zip"
        self.repo.write_ledger(ledger_with(release))
        result = self.repo.run(GENERATOR, "project")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("manifests.url", result.stderr)

    def test_allowlisted_url_is_accepted(self) -> None:
        self.repo.write_channels(dict(CHANNELS, updateHosts=["updates.example.com"]))
        release = release_record(VERSION)
        release["artifacts"][0]["url"] = "https://updates.example.com/app-0.0.1.zip"
        self.repo.write_ledger(ledger_with(release))
        result = self.repo.run(GENERATOR, "project")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        checker = self.repo.run(CHECKER)
        self.assertEqual(checker.returncode, 0, checker.stdout + checker.stderr)
        manifest = json.loads(self.repo.manifest().read_text(encoding="utf-8"))
        self.assertEqual(manifest["url"], "https://updates.example.com/app-0.0.1.zip")

    def test_missing_symbols_block_is_an_error(self) -> None:
        self.seed()
        ledger = self.repo.read_ledger()
        del ledger["releases"][0]["artifacts"][0]["symbols"]
        self.repo.write_ledger(ledger)
        result = self.repo.run(CHECKER)
        self.assertEqual(result.returncode, 1)
        self.assertIn("ledger.symbols", result.stdout)

    def test_signature_without_a_pinned_key_is_an_error(self) -> None:
        self.repo.write_ledger(ledger_with(release_record(VERSION, signature="d" * 128)))
        result = self.repo.run(CHECKER)
        self.assertEqual(result.returncode, 1)
        self.assertIn("ledger.signature", result.stdout)

    def test_release_newer_than_the_project_version_is_an_error(self) -> None:
        self.repo.write_ledger(ledger_with(release_record("1.0.0")))
        self.repo.run(GENERATOR, "project")
        result = self.repo.run(CHECKER)
        self.assertEqual(result.returncode, 1)
        self.assertIn("ledger.project-version", result.stdout)

    def test_json_report_is_stable_and_sorted(self) -> None:
        self.seed()
        first = self.repo.run(CHECKER, "--json")
        second = self.repo.run(CHECKER, "--json")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual(first.stdout, second.stdout, "two --json runs differ")
        payload = json.loads(first.stdout)
        self.assertEqual(payload["gate"], "release-manifest")
        self.assertEqual(payload["channels"], ["stable", "beta"])
        self.assertEqual(payload["pinned_keys"], 0)

    def test_missing_input_is_a_setup_error(self) -> None:
        self.repo.path("release", "ledger.json").unlink()
        result = self.repo.run(CHECKER)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)


class CommittedTreeTests(unittest.TestCase):
    """The real ``release/`` tree in this repository must pass its own gate."""

    def test_repository_tree_passes_the_gate(self) -> None:
        result = subprocess.run(
            [sys.executable, "-B", str(CHECKER), "--json"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=str(REPO_ROOT),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])

    def test_committed_channels_define_stable_and_beta(self) -> None:
        channels = model.load_object(REPO_ROOT / "release" / "channels.json", "channels")
        ids = [channel["id"] for channel in channels["channels"]]
        for required in ("stable", "beta"):
            self.assertIn(required, ids)
        self.assertTrue(any(row["policy"] == "optIn" for row in channels["transitions"]))

    def test_committed_projector_and_gate_agree(self) -> None:
        result = subprocess.run(
            [sys.executable, "-B", str(GENERATOR), "project", "--check"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=str(REPO_ROOT),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
