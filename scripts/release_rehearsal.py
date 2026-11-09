#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Clean-room release rehearsal from the committed tree.

Resolves the exact commit (``git rev-parse HEAD``), creates a detached ``git worktree`` in a
temporary directory, and runs the release sequence there: Release configure (static runtime ON,
Vulkan OFF, ``TESSERA_PACKAGE_EXECUTABLES=<target>``), build, ``cpack`` (TGZ, ZIP on Windows),
then in fixed order ``check_package.py``, ``cmake --install`` + license/SBOM layout verification,
``check_static_link.py``, ``gen_sbom.py --check``, ``gen_license_bundle.py --check`` and
``check_dependencies.py``. Because the worktree contains only committed files, build inputs that
exist only in a dirty working tree make the rehearsal fail instead of slipping into a release.

The caller's working tree and index are never modified; the temporary worktree and install prefix
are removed afterwards unless ``--keep`` is passed.

Exit codes: 0 rehearsal passed, 1 a gate step failed, 2 setup/tool failure (git, configure, build,
cpack, missing artifact). ``--skip-build --artifact PATH`` reuses an existing archive and runs the
package/repository gates only (useful while iterating on the gate steps).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
DEFAULT_TARGET = "merger_tests"
DEFAULT_CONFIG = "Release"
LICENSE_FILES = (
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "THIRD_PARTY_LICENSES.txt",
    "sbom.spdx.json",
)
CONFIGURE_TIMEOUT = 1800
BUILD_TIMEOUT = 3600
GATE_TIMEOUT = 900

if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import check_static_runtime as static_runtime  # noqa: E402  (reuses the MSVC env helper)


@dataclass
class StepResult:
    name: str
    status: str  # "pass" | "fail" | "skip"
    duration_seconds: float
    detail: str = ""


class RehearsalError(Exception):
    """Setup/tool failure (exit code 2)."""


def configure_streams() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(errors="backslashreplace")
            except (ValueError, OSError):
                pass


def child_environment() -> Dict[str, str]:
    environment = dict(os.environ)
    environment["PYTHONIOENCODING"] = "utf-8"
    environment["PYTHONUTF8"] = "1"
    return environment


def run_command(
    command: Sequence[str],
    cwd: Path,
    timeout: int,
    vcvars: Optional[Path],
) -> subprocess.CompletedProcess:
    if vcvars is not None:
        result = static_runtime.run_in_vcvars(command, vcvars, timeout)
        if result.returncode == 127 or "is not recognized" in (result.stdout + result.stderr):
            raise RehearsalError("command not found: {}".format(command[0]))
        return result
    try:
        return subprocess.run(
            list(command),
            cwd=str(cwd),
            env=child_environment(),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
    except FileNotFoundError:
        raise RehearsalError("command not found: {}".format(command[0]))
    except subprocess.SubprocessError as error:
        raise RehearsalError("command failed: {}".format(error))


def run_git(
    git: str, repo_root: Path, args: Sequence[str], timeout: int = 120
) -> subprocess.CompletedProcess:
    return run_command([git, *args], repo_root, timeout, None)


def require_ok(result: subprocess.CompletedProcess, label: str) -> str:
    output = (result.stdout + result.stderr).strip()
    if result.returncode != 0:
        raise RehearsalError("{} failed (exit {}): {}".format(label, result.returncode, output))
    return output


def resolve_head(git: str, repo_root: Path) -> str:
    try:
        result = run_git(git, repo_root, ["rev-parse", "HEAD"])
    except RehearsalError as error:
        raise RehearsalError("git unavailable or not a repository: {}".format(error))
    return require_ok(result, "git rev-parse HEAD").strip()


def create_worktree(git: str, repo_root: Path, destination: Path, sha: str) -> None:
    result = run_git(git, repo_root, ["worktree", "add", "--detach", str(destination), sha])
    require_ok(result, "git worktree add")


def remove_worktree(git: str, repo_root: Path, destination: Path) -> None:
    if not destination.exists():
        return
    try:
        run_git(git, repo_root, ["worktree", "remove", "--force", str(destination)])
    finally:
        if destination.exists():
            shutil.rmtree(destination, ignore_errors=True)
        try:
            run_git(git, repo_root, ["worktree", "prune"])
        except RehearsalError:
            pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def find_archive(artifact_dir: Path) -> Path:
    patterns = ("tessera-*.zip", "tessera-*.tar.gz")
    matches: List[Path] = []
    for pattern in patterns:
        matches.extend(sorted(artifact_dir.glob(pattern)))
    if not matches:
        raise RehearsalError("cpack did not produce a tessera-*.zip/.tar.gz in {}".format(artifact_dir))
    return matches[0]


def find_binary(build_dir: Path, target: str) -> Path:
    names = (target, target + ".exe")
    matches = [
        path
        for name in names
        for path in sorted(build_dir.rglob(name))
        if path.is_file() and "tessera-package-root" not in path.parts
    ]
    if not matches:
        raise RehearsalError(
            "build did not produce a '{}' executable under {}".format(target, build_dir)
        )
    return matches[0]


class Rehearsal:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.repo_root = Path(args.repo_root).resolve()
        self.steps: List[StepResult] = []
        self.worktree: Optional[Path] = None
        self.install_prefix: Optional[Path] = None
        self.artifact: Optional[Path] = None
        self.binary: Optional[Path] = None
        self.head = ""

    def add(self, name: str, status: str, duration: float, detail: str = "") -> None:
        self.steps.append(StepResult(name, status, duration, detail))

    def run_command_step(
        self,
        name: str,
        command: Sequence[str],
        cwd: Path,
        timeout: int,
        vcvars: Optional[Path],
        gate: bool = True,
    ) -> bool:
        started = time.perf_counter()
        if not self.args.as_json:
            print("== step: {} ==".format(name))
        try:
            result = run_command(command, cwd, timeout, vcvars)
        except RehearsalError as error:
            self.add(name, "fail" if gate else "setup", time.perf_counter() - started, str(error))
            print("error: {}".format(error), file=sys.stderr)
            return False
        duration = time.perf_counter() - started
        output = (result.stdout + result.stderr).strip()
        if output and not self.args.as_json:
            print(output)
        if result.returncode != 0:
            self.add(
                name,
                "fail" if gate else "setup",
                duration,
                "exit {}{}".format(
                    result.returncode, ": " + output.splitlines()[-1] if output else ""
                ),
            )
            return False
        self.add(name, "pass", duration)
        return True

    def prepare(self) -> None:
        temporary = tempfile.mkdtemp(prefix="tessera-release-rehearsal-")
        self.temporary_root = Path(temporary)
        self.worktree = self.temporary_root / "worktree"
        self.install_prefix = self.temporary_root / "install"
        vcvars = static_runtime.vcvars_environment() if os.name == "nt" else None

        try:
            self.head = resolve_head(self.args.git, self.repo_root)
        except RehearsalError:
            shutil.rmtree(self.temporary_root, ignore_errors=True)
            raise

        started = time.perf_counter()
        try:
            create_worktree(self.args.git, self.repo_root, self.worktree, self.head)
        except RehearsalError:
            shutil.rmtree(self.temporary_root, ignore_errors=True)
            raise
        self.add("worktree", "pass", time.perf_counter() - started, self.head)

        build_dir = self.worktree / "build" / "release-rehearsal"
        configure = [
            self.args.cmake,
            "-S",
            str(self.worktree),
            "-B",
            str(build_dir),
            "-DCMAKE_BUILD_TYPE={}".format(self.args.config),
            "-DTESSERA_STATIC_RUNTIME=ON",
            "-DTESSERA_BUILD_VULKAN=OFF",
            "-DTESSERA_BUILD_POC=OFF",
            "-DTESSERA_BUILD_TESTS=ON",
            "-DTESSERA_USE_COMPILER_CACHE=OFF",
            "-DTESSERA_PACKAGE_EXECUTABLES={}".format(self.args.target),
        ]
        if not self.run_command_step(
            "configure", configure, self.worktree, CONFIGURE_TIMEOUT, vcvars, gate=False
        ):
            raise RehearsalError("configure failed")

        # The worktree must still contain only committed files after configure; generated files are
        # expected inside the build directory only. Anything else means configure wrote into the
        # source tree, which the release must not depend on.
        started = time.perf_counter()
        status = run_git(self.args.git, self.worktree, ["status", "--porcelain"])
        require_ok(status, "git status in worktree")
        outside = []
        for line in status.stdout.splitlines():
            path = line[3:].strip().strip('"')
            parts = Path(path).parts
            if parts and (parts[0] == "build" or parts[0].startswith("build")):
                continue
            outside.append(path)
        duration = time.perf_counter() - started
        if outside:
            self.add(
                "clean-tree",
                "setup",
                duration,
                "configure wrote outside the build directory: {}".format(", ".join(outside[:5])),
            )
            raise RehearsalError("configure modified the source tree")
        self.add("clean-tree", "pass", duration)

        build = [
            self.args.cmake,
            "--build",
            str(build_dir),
            "--config",
            self.args.config,
            "--target",
            "tessera-package-stage",
            "--parallel",
        ]
        if not self.run_command_step(
            "build", build, self.worktree, BUILD_TIMEOUT, vcvars, gate=False
        ):
            raise RehearsalError("build failed")
        self.binary = find_binary(build_dir, self.args.target)

        artifact_dir = build_dir / "package-out"
        artifact_dir.mkdir(parents=True, exist_ok=True)
        generator = "ZIP" if os.name == "nt" else "TGZ"
        cpack_command = [
            self.args.cpack,
            "--config",
            str(build_dir / "CPackConfig.cmake"),
            "-G",
            generator,
            "-C",
            self.args.config,
            "-B",
            str(artifact_dir),
        ]
        if not self.run_command_step(
            "cpack", cpack_command, self.worktree, GATE_TIMEOUT, vcvars, gate=False
        ):
            raise RehearsalError("cpack failed")
        self.artifact = find_archive(artifact_dir)
        self.build_dir = build_dir
        self.vcvars = vcvars

    def run_steps(self) -> None:
        assert self.artifact is not None and self.binary is not None
        assert self.worktree is not None and self.install_prefix is not None
        scripts = self.worktree / "scripts"
        python = sys.executable
        common = [python, "-B"]

        self.run_command_step(
            "package",
            common + [str(scripts / "check_package.py"), "--archive", str(self.artifact)],
            self.worktree,
            GATE_TIMEOUT,
            None,
        )

        install = [
            self.args.cmake,
            "--install",
            str(self.build_dir),
            "--config",
            self.args.config,
            "--prefix",
            str(self.install_prefix),
            "--component",
            "licenses",
        ]
        started = time.perf_counter()
        try:
            result = run_command(install, self.worktree, GATE_TIMEOUT, self.vcvars)
            if result.returncode != 0:
                self.add(
                    "install",
                    "fail",
                    time.perf_counter() - started,
                    "cmake --install failed (exit {})".format(result.returncode),
                )
            else:
                missing = [
                    name
                    for name in LICENSE_FILES
                    if not (self.install_prefix / "share" / "tessera" / "licenses" / name).is_file()
                ]
                self.add(
                    "install",
                    "fail" if missing else "pass",
                    time.perf_counter() - started,
                    "missing: {}".format(", ".join(missing)) if missing else "",
                )
        except RehearsalError as error:
            self.add("install", "fail", time.perf_counter() - started, str(error))

        self.run_command_step(
            "static-link",
            common
            + [
                str(scripts / "check_static_link.py"),
                "--os",
                "auto",
                "--binary",
                str(self.binary),
            ],
            self.worktree,
            GATE_TIMEOUT,
            None,
        )
        self.run_command_step(
            "sbom",
            common + [str(scripts / "gen_sbom.py"), "--check"],
            self.worktree,
            GATE_TIMEOUT,
            None,
        )
        self.run_command_step(
            "license-bundle",
            common + [str(scripts / "gen_license_bundle.py"), "--check"],
            self.worktree,
            GATE_TIMEOUT,
            None,
        )
        self.run_command_step(
            "dependencies",
            common + [str(scripts / "check_dependencies.py")],
            self.worktree,
            GATE_TIMEOUT,
            None,
        )

    def run_skip_build(self) -> None:
        self.artifact = Path(self.args.artifact)
        scripts = self.repo_root / "scripts"
        python = sys.executable
        common = [python, "-B"]
        self.run_command_step(
            "package",
            common + [str(scripts / "check_package.py"), "--archive", str(self.artifact)],
            self.repo_root,
            GATE_TIMEOUT,
            None,
        )
        for name in ("install", "static-link"):
            self.add(name, "skip", 0.0, "--skip-build")
        for name, command in (
            ("sbom", common + [str(scripts / "gen_sbom.py"), "--check"]),
            ("license-bundle", common + [str(scripts / "gen_license_bundle.py"), "--check"]),
            ("dependencies", common + [str(scripts / "check_dependencies.py")]),
        ):
            self.run_command_step(name, command, self.repo_root, GATE_TIMEOUT, None)

    def cleanup(self) -> None:
        if self.args.keep:
            if self.worktree is not None:
                print("kept worktree: {}".format(self.worktree))
            if self.install_prefix is not None:
                print("kept install prefix: {}".format(self.install_prefix))
            if self.artifact is not None:
                print("artifact: {}".format(self.artifact))
            return
        if self.worktree is not None and self.worktree.exists():
            try:
                remove_worktree(self.args.git, self.repo_root, self.worktree)
            except RehearsalError as error:
                print("warning: could not remove worktree: {}".format(error), file=sys.stderr)
        if getattr(self, "temporary_root", None) is not None:
            shutil.rmtree(self.temporary_root, ignore_errors=True)

    def summary(self, total_seconds: float) -> Dict[str, object]:
        passed = sum(1 for step in self.steps if step.status == "pass")
        failed = sum(1 for step in self.steps if step.status == "fail")
        setup = sum(1 for step in self.steps if step.status == "setup")
        skipped = sum(1 for step in self.steps if step.status == "skip")
        if setup:
            exit_code = 2
        elif failed:
            exit_code = 1
        else:
            exit_code = 0
        artifact_size = self.artifact.stat().st_size if self.artifact and self.artifact.is_file() else 0
        artifact_hash = sha256_file(self.artifact) if self.artifact and self.artifact.is_file() else ""
        payload: Dict[str, object] = {
            "ok": exit_code == 0,
            "gate": "release-rehearsal",
            "head": self.head,
            "target": self.args.target,
            "artifact": {
                "path": str(self.artifact) if self.artifact else "",
                "size": artifact_size,
                "sha256": artifact_hash,
            },
            "steps": [
                {
                    "name": step.name,
                    "status": step.status,
                    "duration_seconds": round(step.duration_seconds, 3),
                    "detail": step.detail,
                }
                for step in self.steps
            ],
            "total_seconds": round(total_seconds, 2),
            "exit_code": exit_code,
        }
        return payload


def print_summary(payload: Dict[str, object]) -> None:
    print("")
    print("Release rehearsal summary (HEAD {})".format(str(payload["head"])[:12]))
    print("---------------------------------------------")
    print("{:<15} {:<6} {:>8}".format("step", "status", "duration"))
    for step in payload["steps"]:
        print(
            "{:<15} {:<6} {:>8.2f}s".format(
                step["name"], step["status"].upper(), step["duration_seconds"]
            )
        )
    artifact = payload["artifact"]
    print("")
    print(
        "artifact: {} ({} bytes, sha256 {})".format(
            artifact["path"], artifact["size"], artifact["sha256"] or "-"
        )
    )
    print("total: {:.2f}s".format(payload["total_seconds"]))
    print(
        "{} step(s), exit {}".format(len(payload["steps"]), payload["exit_code"])
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    configure_streams()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument("--target", default=DEFAULT_TARGET)
    parser.add_argument("--config", choices=("Release", "Debug"), default=DEFAULT_CONFIG)
    parser.add_argument("--git", default="git")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--cpack", default="cpack")
    parser.add_argument("--keep", action="store_true", help="keep the worktree and install prefix")
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="skip worktree/build/cpack and run the package/repository gates only",
    )
    parser.add_argument("--artifact", default=None, help="existing archive for --skip-build")
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    if args.skip_build and not args.artifact:
        print("error: --skip-build requires --artifact PATH", file=sys.stderr)
        return 2
    if args.artifact and not Path(args.artifact).is_file():
        print("error: artifact not found: {}".format(args.artifact), file=sys.stderr)
        return 2

    started = time.perf_counter()
    rehearsal = Rehearsal(args)
    exit_code = 0
    try:
        if args.skip_build:
            rehearsal.artifact = Path(args.artifact)
            rehearsal.head = "skip-build"
            rehearsal.run_skip_build()
        else:
            rehearsal.prepare()
            rehearsal.run_steps()
    except RehearsalError as error:
        print("error: {}".format(error), file=sys.stderr)
        exit_code = 2
    except Exception as error:  # noqa: BLE001 - the rehearsal must always clean up
        print("error: unexpected failure: {}".format(error), file=sys.stderr)
        exit_code = 2
    finally:
        total_seconds = time.perf_counter() - started
        try:
            payload = rehearsal.summary(total_seconds)
        except Exception:  # noqa: BLE001 - never mask the original failure
            payload = {
                "ok": False,
                "gate": "release-rehearsal",
                "head": rehearsal.head,
                "steps": [],
                "exit_code": 2,
            }
        if args.as_json:
            print(json.dumps(payload, indent=2, sort_keys=True))
        else:
            print_summary(payload)
        rehearsal.cleanup()

    if exit_code != 0 and int(payload.get("exit_code", 0)) != exit_code:
        payload["ok"] = False
        payload["exit_code"] = exit_code
    return int(payload.get("exit_code", exit_code))


if __name__ == "__main__":
    sys.exit(main())
