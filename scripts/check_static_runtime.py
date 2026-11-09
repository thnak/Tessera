#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Release static-runtime proof for ADR-012.

The task-8 static-link gate only inspects whatever artifacts already exist (usually Debug test
binaries). This script proves the hard requirement end to end: it configures a throwaway build
directory with ``TESSERA_STATIC_RUNTIME=ON``, ``CMAKE_BUILD_TYPE=Release`` and
``TESSERA_BUILD_VULKAN=OFF``, builds one small framework-linking target (``merger_tests`` by
default), and runs ``scripts/check_static_link.py`` on the produced executable.

Debug builds are reported as ``SKIPPED`` instead of passing or failing: MSVC ``/MTd`` and debug
C++ runtimes are not fully static (they still import ``VCRUNTIME*D``/``MSVCP*D``/``ucrtbased``),
so the proof applies to Release (``/MT``, ``-static-libstdc++ -static-libgcc``) only.

Exit codes: 0 = pass (or Debug SKIPPED), 1 = static-link violation, 2 = tool/setup failure.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
GATE = SCRIPTS_DIR / "check_static_link.py"
DEFAULT_BUILD_DIR = REPO_ROOT / "build" / "static-runtime-proof"
DEFAULT_TARGET = "merger_tests"
DEFAULT_CONFIG = "Release"

if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from check_static_link import find_vcvars64  # noqa: E402  (sibling script, stdlib only)

CONFIGURE_TIMEOUT = 900
BUILD_TIMEOUT = 1800
GATE_TIMEOUT = 600


def fail_setup(message: str) -> "NoReturn":
    print("error: {}".format(message), file=sys.stderr)
    raise SystemExit(2)


def is_debug_request(config: str) -> bool:
    return config.strip().lower() == "debug"


def looks_like_debug_path(path: Path) -> bool:
    return any(part.lower() == "debug" for part in path.parts)


def print_skipped(reason: str) -> int:
    print(
        "SKIPPED: {}. Debug builds use the debug C/C++ runtime (MSVC /MTd, debug libstdc++), "
        "which is not fully static.".format(reason)
    )
    print(
        "The static-runtime proof applies to Release (/MT, -static-libstdc++ -static-libgcc); "
        "use --config Release and a Release artifact."
    )
    return 0


def vcvars_environment() -> Optional[Path]:
    """On Windows, the CMake/NMake toolchain needs the MSVC developer environment."""
    if os.name != "nt":
        return None
    if shutil.which("cl") and shutil.which("nmake"):
        return None
    return find_vcvars64(None)


def quote_argument(argument: str) -> str:
    return '"{}"'.format(argument) if any(character.isspace() for character in argument) else argument


def run_in_vcvars(command: Sequence[str], vcvars: Path, timeout: int) -> subprocess.CompletedProcess:
    handle = tempfile.NamedTemporaryFile(
        "w", suffix=".bat", delete=False, encoding="ascii", newline="\r\n"
    )
    try:
        handle.write("@echo off\n")
        handle.write('call "{}" >nul\n'.format(vcvars))
        handle.write("{}\n".format(" ".join(quote_argument(argument) for argument in command)))
        handle.write("exit /b %ERRORLEVEL%\n")
        handle.close()
        return subprocess.run(
            ["cmd", "/d", "/c", handle.name], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout
        )
    finally:
        try:
            os.unlink(handle.name)
        except OSError:
            pass


def run_command(
    command: Sequence[str],
    label: str,
    timeout: int,
    vcvars: Optional[Path] = None,
) -> subprocess.CompletedProcess:
    print("{}: {}".format(label, " ".join(command)))
    try:
        if vcvars is not None:
            result = run_in_vcvars(command, vcvars, timeout)
        else:
            result = subprocess.run(
                list(command), capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout
            )
    except FileNotFoundError:
        fail_setup("{} not found; install CMake or pass --cmake PATH".format(command[0]))
    except subprocess.SubprocessError as error:
        fail_setup("{} failed: {}".format(label, error))
    output = (result.stdout + result.stderr).strip()
    if result.returncode != 0:
        if output:
            print(output, file=sys.stderr)
        fail_setup("{} failed (exit {})".format(label, result.returncode))
    if output:
        print(output)
    return result


def configure(
    build_dir: Path,
    config: str,
    cmake: str,
    generator: Optional[str],
    vcvars: Optional[Path] = None,
) -> None:
    command = [
        cmake,
        "-S",
        str(REPO_ROOT),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE={}".format(config),
        "-DTESSERA_STATIC_RUNTIME=ON",
        "-DTESSERA_BUILD_VULKAN=OFF",
        "-DTESSERA_BUILD_POC=OFF",
        "-DTESSERA_BUILD_TESTS=ON",
        "-DTESSERA_USE_COMPILER_CACHE=OFF",
        "-DTESSERA_WARNINGS_AS_ERRORS=ON",
    ]
    if generator:
        command.extend(["-G", generator])
    run_command(command, "configure", CONFIGURE_TIMEOUT, vcvars)


def build(
    build_dir: Path,
    config: str,
    target: str,
    cmake: str,
    vcvars: Optional[Path] = None,
) -> None:
    command = [
        cmake,
        "--build",
        str(build_dir),
        "--config",
        config,
        "--target",
        target,
        "--parallel",
    ]
    run_command(command, "build", BUILD_TIMEOUT, vcvars)


def find_artifact(build_dir: Path, target: str, config: str) -> Optional[Path]:
    exe = target + (".exe" if os.name == "nt" else "")
    matches = [path for path in build_dir.rglob(exe) if path.is_file()]
    if not matches:
        return None
    preferred = [
        path
        for path in matches
        if any(part.lower() == config.lower() for part in path.parts)
    ]
    candidates = preferred or matches
    return sorted(candidates, key=lambda path: (len(path.parts), str(path)))[0]


def gate_command(args: argparse.Namespace, artifact: Optional[Path], from_output: Optional[Path]) -> List[str]:
    command = [sys.executable, "-B", str(GATE), "--os", args.os]
    if from_output is not None:
        command.extend(["--from-output", str(from_output)])
    else:
        command.extend(["--binary", str(artifact)])
    if args.tool != "auto":
        command.extend(["--tool", args.tool])
    if args.dumpbin:
        command.extend(["--dumpbin", args.dumpbin])
    if args.allowlist:
        command.extend(["--allowlist", args.allowlist])
    return command


def run_gate(command: Sequence[str]) -> int:
    try:
        result = subprocess.run(
            list(command), capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=GATE_TIMEOUT
        )
    except FileNotFoundError:
        fail_setup("Python interpreter for the static-link gate is not available")
    except subprocess.SubprocessError as error:
        fail_setup("static-link gate failed: {}".format(error))
    output = (result.stdout + result.stderr).strip()
    if output:
        print(output)
    if result.returncode == 0:
        return 0
    if result.returncode == 1:
        return 1
    fail_setup("static-link gate could not inspect the artifact (exit {})".format(result.returncode))


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", default=DEFAULT_CONFIG, help="CMake configuration (default: Release)")
    parser.add_argument("--target", default=DEFAULT_TARGET, help="CMake target to build (default: merger_tests)")
    parser.add_argument(
        "--build-dir",
        default=str(DEFAULT_BUILD_DIR),
        help="throwaway build directory (default: build/static-runtime-proof)",
    )
    parser.add_argument("--artifact", default=None, help="inspect this executable instead of building")
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="skip configure/build; requires --artifact",
    )
    parser.add_argument(
        "--from-output",
        default=None,
        metavar="FILE",
        help="parse captured ldd/otool/dumpbin output (fixture mode; no build)",
    )
    parser.add_argument("--os", choices=("auto", "linux", "macos", "windows"), default="auto")
    parser.add_argument("--tool", choices=("auto", "ldd", "otool", "dumpbin"), default="auto")
    parser.add_argument("--dumpbin", default=None, help="explicit dumpbin.exe path (Windows)")
    parser.add_argument("--allowlist", default=None, help="replace the static-link allowlist file")
    parser.add_argument("--cmake", default="cmake", help="cmake executable (default: cmake)")
    parser.add_argument("--generator", default=None, help="optional CMake generator override")
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    config = args.config
    artifact = Path(args.artifact) if args.artifact else None

    if is_debug_request(config):
        return print_skipped("--config {} requests a debug build".format(config))
    if artifact is not None and looks_like_debug_path(artifact):
        return print_skipped(
            "artifact {} is under a Debug directory".format(artifact)
        )

    print("Static-runtime proof ({} target, TESSERA_STATIC_RUNTIME=ON)".format(args.target))

    from_output = Path(args.from_output) if args.from_output else None
    if from_output is not None:
        if not from_output.is_file():
            fail_setup("tool output not found: {}".format(from_output))
        print("  output:   {}".format(from_output))
        return run_gate(gate_command(args, None, from_output))

    if args.skip_build and artifact is None:
        fail_setup("--skip-build requires --artifact PATH")

    if artifact is None:
        build_dir = Path(args.build_dir)
        print("  build:    {}".format(build_dir))
        vcvars = vcvars_environment()
        if vcvars is not None:
            print("  vcvars:   {}".format(vcvars))
        configure(build_dir, config, args.cmake, args.generator, vcvars)
        build(build_dir, config, args.target, args.cmake, vcvars)
        artifact = find_artifact(build_dir, args.target, config)
        if artifact is None:
            fail_setup(
                "build succeeded but no {!r} artifact was found under {}; pass --artifact PATH"
                .format(args.target, build_dir)
            )
    if not artifact.is_file():
        fail_setup("artifact not found: {}".format(artifact))

    print("  artifact: {}".format(artifact))
    result = run_gate(gate_command(args, artifact, None))
    if result == 0:
        print("PASS: the {} artifact links the C/C++ runtime statically.".format(config))
    else:
        print("FAIL: the {} artifact has static-link violations (see above).".format(config))
    return result


if __name__ == "__main__":
    sys.exit(main())
