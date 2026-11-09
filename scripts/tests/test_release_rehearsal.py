#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixture tests for scripts/release_rehearsal.py.

A tiny fake repository plus fake ``cmake``/``cpack`` wrappers are used so the rehearsal's
worktree isolation, step order, summary, cleanup and failure semantics are covered without a real
compiler or network access. The fake cmake refuses to configure when the committed tree is missing
a required file, which is exactly the "input only exists in the dirty working tree" class.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v
"""

from __future__ import annotations

import json
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
REHEARSAL = SCRIPTS_DIR / "release_rehearsal.py"
GATE_SCRIPTS = (
    "check_package.py",
    "check_static_link.py",
    "gen_sbom.py",
    "gen_license_bundle.py",
    "check_dependencies.py",
)

CMAKE_STUB = '''\
import pathlib
import sys

args = sys.argv[1:]


def value(flag):
    return args[args.index(flag) + 1] if flag in args else None


if "--install" in args:
    prefix = pathlib.Path(value("--prefix"))
    licenses = prefix / "share" / "tessera" / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "THIRD_PARTY_LICENSES.txt", "sbom.spdx.json"):
        (licenses / name).write_text("fixture\\n", encoding="utf-8")
    sys.exit(0)

if "--build" in args:
    build = pathlib.Path(value("--build"))
    build.mkdir(parents=True, exist_ok=True)
    # The real build target (tessera-package-stage) produces the shipped executable.
    suffix = ".exe" if sys.platform == "win32" else ""
    (build / ("merger_tests" + suffix)).write_bytes(b"fake binary")
    sys.exit(0)

if "-S" in args:
    source = pathlib.Path(value("-S"))
    if not (source / "required.txt").is_file():
        print("missing required.txt in " + str(source), file=sys.stderr)
        sys.exit(1)
    sys.exit(0)

sys.exit(0)
'''

CPACK_STUB = '''\
import pathlib
import sys

args = sys.argv[1:]
out = pathlib.Path(args[args.index("-B") + 1])
out.mkdir(parents=True, exist_ok=True)
suffix = ".zip" if sys.platform == "win32" else ".tar.gz"
(out / ("tessera-0.0.0-fixture" + suffix)).write_bytes(b"fake archive")
sys.exit(0)
'''


class RehearsalTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.dir = Path(self._temp.name)
        self.repo = self.dir / "repo"
        self.bin_dir = self.dir / "bin"
        self.bin_dir.mkdir()
        self._write_tool("cmake", CMAKE_STUB)
        self._write_tool("cpack", CPACK_STUB)

    def tearDown(self) -> None:
        self._temp.cleanup()

    # -- fixture helpers ----------------------------------------------------------------
    def _write_tool(self, name: str, body: str) -> None:
        implementation = self.bin_dir / (name + "_impl.py")
        implementation.write_text(body, encoding="utf-8")
        if os.name == "nt":
            wrapper = self.bin_dir / (name + ".bat")
            wrapper.write_text(
                '@python "%~dp0{}_impl.py" %*\n'.format(name), encoding="ascii"
            )
        else:
            wrapper = self.bin_dir / name
            wrapper.write_text(
                '#!/bin/sh\nexec python "$(dirname "$0")/{}_impl.py" "$@"\n'.format(name),
                encoding="ascii",
            )
            wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)

    def _make_repo(self, include_required: bool = True, with_git: bool = True) -> None:
        (self.repo / "scripts").mkdir(parents=True)
        (self.repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\nproject(rehearsal_fixture LANGUAGES NONE)\n",
            encoding="utf-8",
        )
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "THIRD_PARTY_LICENSES.txt", "sbom.spdx.json"):
            (self.repo / name).write_text("fixture\n", encoding="utf-8")
        for name in GATE_SCRIPTS:
            (self.repo / "scripts" / name).write_text(
                "import sys\nsys.exit(0)\n", encoding="utf-8"
            )
        if include_required:
            (self.repo / "required.txt").write_text("needed by the fake build\n", encoding="utf-8")
        if with_git:
            self._git("init", "-q")
            self._git("add", ".")
            self._git(
                "-c",
                "user.email=rehearsal@example.invalid",
                "-c",
                "user.name=rehearsal",
                "commit",
                "-q",
                "-m",
                "fixture",
            )
            if not include_required:
                # The file exists only in the caller's working tree, never in HEAD.
                (self.repo / "required.txt").write_text("dirty-only input\n", encoding="utf-8")

    def _git(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["git", *args],
            cwd=str(self.repo),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

    def _run(self, *extra: str, json_mode: bool = False) -> subprocess.CompletedProcess:
        environment = dict(os.environ)
        environment["PATH"] = str(self.bin_dir) + os.pathsep + environment.get("PATH", "")
        command = [
            sys.executable,
            "-B",
            str(REHEARSAL),
            "--repo-root",
            str(self.repo),
            "--cmake",
            "cmake",
            "--cpack",
            "cpack",
            "--target",
            "merger_tests",
        ]
        if json_mode:
            command.append("--json")
        command.extend(extra)
        return subprocess.run(
            command,
            cwd=str(REPO_ROOT),
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

    def _worktree_count(self) -> int:
        result = self._git("worktree", "list", "--porcelain")
        if result.returncode != 0:
            return -1
        return sum(1 for line in result.stdout.splitlines() if line.startswith("worktree "))

    # -- tests --------------------------------------------------------------------------
    def test_clean_committed_tree_passes(self) -> None:
        self._make_repo(include_required=True)
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("worktree", result.stdout)
        for name in ("configure", "clean-tree", "build", "cpack", "package", "install",
                     "static-link", "sbom", "license-bundle", "dependencies"):
            self.assertIn(name, result.stdout)
        self.assertIn("artifact:", result.stdout)
        self.assertEqual(self._worktree_count(), 1)

    def test_untracked_required_input_fails(self) -> None:
        self._make_repo(include_required=False)
        result = self._run()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("configure failed", (result.stdout + result.stderr).lower())
        self.assertEqual(self._worktree_count(), 1)  # cleaned up, only the main worktree remains

    def test_missing_git_is_a_setup_error(self) -> None:
        self._make_repo(with_git=False)
        result = self._run()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("git", (result.stdout + result.stderr).lower())

    def test_skip_build_without_artifact_is_a_usage_error(self) -> None:
        self._make_repo()
        result = self._run("--skip-build")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("--artifact", result.stderr)

    def test_skip_build_with_missing_artifact_is_an_error(self) -> None:
        self._make_repo()
        result = self._run("--skip-build", "--artifact", str(self.dir / "nope.tar.gz"))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("artifact not found", result.stderr)

    def test_skip_build_runs_package_and_repository_gates(self) -> None:
        self._make_repo()
        artifact = self.dir / "existing.tar.gz"
        artifact.write_bytes(b"fixture archive")
        result = self._run("--skip-build", "--artifact", str(artifact))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("package", result.stdout)
        self.assertIn("SKIP", result.stdout)
        self.assertIn("install", result.stdout)
        self.assertIn("static-link", result.stdout)

    def test_json_summary_for_clean_rehearsal(self) -> None:
        self._make_repo()
        result = self._run(json_mode=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["gate"], "release-rehearsal")
        self.assertEqual(len(payload["head"]), 40)
        steps = {step["name"]: step["status"] for step in payload["steps"]}
        self.assertEqual(steps["package"], "pass")
        self.assertEqual(steps["install"], "pass")
        self.assertGreater(payload["artifact"]["size"], 0)
        self.assertEqual(payload["exit_code"], 0)

    def test_keep_preserves_the_worktree_for_inspection(self) -> None:
        self._make_repo(include_required=False)
        result = self._run("--keep")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("kept worktree:", result.stdout)
        self.assertGreaterEqual(self._worktree_count(), 2)
        kept = [
            line.split("kept worktree:", 1)[1].strip()
            for line in result.stdout.splitlines()
            if line.startswith("kept worktree:")
        ]
        self.assertEqual(len(kept), 1)
        self._git("worktree", "remove", "--force", kept[0])
        self.assertEqual(self._worktree_count(), 1)


if __name__ == "__main__":
    unittest.main()
