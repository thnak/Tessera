#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reproducible-build gate.

Builds one small, stable target twice in two separate build directories from the same source tree
with the same toolchain and with compiler caching disabled, then requires the two artifacts to be
byte-identical (same SHA-256). On a mismatch it reports the first differing offset, the number and
size of differing regions, and a heuristic cause: PE/COFF timestamp, embedded absolute path, or
section/layout ordering.

``--package`` compares the CPack archives produced by the two builds instead of the raw
executables; cpack runs with a fixed ``SOURCE_DATE_EPOCH`` and ``TZ=UTC`` so archive entry
mtimes cannot leak build time into the bytes.

``--compare A B`` skips the build and compares two existing artifacts, which is what the fixture
tests use. A build run needs cmake (and the platform compiler); on Windows the MSVC environment is
bootstrapped through ``vcvars64.bat`` when needed.

Exit codes: 0 identical, 1 artifacts differ, 2 setup/tool failure.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
DEFAULT_BUILD_ROOT = REPO_ROOT / "build" / "reproducible"
DEFAULT_TARGET = "merger_tests"
DEFAULT_CONFIG = "Release"
# Fixed archive timestamps: 1980-01-01T00:00:00Z is the minimum ZIP DOS timestamp.
DEFAULT_ARCHIVE_EPOCH = "315532800"
DEFAULT_ARCHIVE_TZ = "UTC"

if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import check_static_runtime as static_runtime  # noqa: E402  (reuses vcvars + artifact lookup)

CONFIGURE_TIMEOUT = 900
BUILD_TIMEOUT = 1800
COMPARE_CONTEXT = 48
_JSON_MODE = False


def info(message: str) -> None:
    # With --json, stdout must stay machine-readable; progress goes to stderr.
    print(message, file=sys.stderr if _JSON_MODE else sys.stdout)


class ReproError(Exception):
    """Setup/tool failure (exit code 2)."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pe_timestamp_offset(data: bytes) -> Optional[int]:
    """Return the PE/COFF TimeDateStamp offset for a Windows executable, if this is one."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        return None
    # COFF header starts right after the 4-byte PE signature:
    # Machine(2) NumberOfSections(2) TimeDateStamp(4) ...
    return pe_offset + 4 + 4


def region_contains_timestamp(
    a: bytes, b: bytes, start: int, end: int, timestamp_a: int, timestamp_b: int
) -> bool:
    """True when the differing region contains the link timestamp value in both files."""
    for offset in range(max(0, start - 3), end + 1):
        if offset + 4 > len(a) or offset + 4 > len(b):
            continue
        if struct.unpack_from("<I", a, offset)[0] == timestamp_a and struct.unpack_from(
            "<I", b, offset
        )[0] == timestamp_b:
            return True
    return False


def printable_context(data: bytes, offset: int, radius: int = COMPARE_CONTEXT) -> str:
    start = max(0, offset - radius)
    end = min(len(data), offset + radius)
    chunk = data[start:end]
    printable = "".join(
        chr(byte) if 32 <= byte < 127 else "." for byte in chunk
    )
    return printable


def differing_regions(a: bytes, b: bytes, merge_gap: int = 8) -> List[Tuple[int, int, int]]:
    """Return (start, end, count) regions of differing bytes (end exclusive)."""
    regions: List[Tuple[int, int, int]] = []
    limit = min(len(a), len(b))
    start: Optional[int] = None
    last: Optional[int] = None
    count = 0
    for offset in range(limit):
        if a[offset] != b[offset]:
            if start is None:
                start = offset
            elif last is not None and offset - last > merge_gap:
                regions.append((start, last + 1, count))
                start = offset
                count = 0
            last = offset
            count += 1
    if start is not None and last is not None:
        regions.append((start, last + 1, count))
    if len(a) != len(b):
        regions.append((limit, max(len(a), len(b)), abs(len(a) - len(b))))
    return regions


def looks_like_path(text: str) -> bool:
    if "/" not in text and "\\" not in text:
        return False
    return any(token in text for token in ("/", "\\", "build", "src", "Users", "home", "tmp"))


def classify_difference(
    a: bytes, b: bytes, regions: Sequence[Tuple[int, int, int]]
) -> Tuple[str, str]:
    """Return (classification, human-readable detail)."""
    if not regions:
        return "identical", "no differing bytes"
    timestamp = pe_timestamp_offset(a)
    timestamp_b = pe_timestamp_offset(b)
    if timestamp is not None and timestamp_b is not None:
        timestamp_a_value = struct.unpack_from("<I", a, timestamp)[0]
        timestamp_b_value = struct.unpack_from("<I", b, timestamp)[0]
        if timestamp_a_value != timestamp_b_value:
            all_timestamp = True
            for start, end, _count in regions:
                if timestamp <= start < timestamp + 4:
                    continue
                if not region_contains_timestamp(
                    a, b, start, end, timestamp_a_value, timestamp_b_value
                ):
                    all_timestamp = False
                    break
            if all_timestamp:
                return (
                    "timestamp",
                    "PE/COFF link timestamp at offset 0x{:x} differs (0x{:08x} vs 0x{:08x}) and "
                    "also appears in the debug directory; link with /Brepro (MSVC) or make the "
                    "toolchain SOURCE_DATE_EPOCH-aware".format(
                        timestamp, timestamp_a_value, timestamp_b_value
                    ),
                )

    path_regions = 0
    contexts: List[str] = []
    for start, end, _count in regions[:16]:
        context_a = printable_context(a, start)
        context_b = printable_context(b, start)
        if context_a != context_b and looks_like_path(context_a) and looks_like_path(context_b):
            path_regions += 1
            if len(contexts) < 3:
                contexts.append(context_a + "  !=  " + context_b)
    if path_regions:
        return (
            "absolute-path",
            "{} differing region(s) look like embedded paths; map source/build roots with "
            "-ffile-prefix-map/-fdebug-prefix-map (GCC/Clang) or /PATHMAP (MSVC). Example: {}".format(
                path_regions, contexts[0] if contexts else ""
            ),
        )

    if len(a) != len(b):
        return (
            "ordering",
            "artifact sizes differ ({} vs {} bytes); content/layout is not stable".format(
                len(a), len(b)
            ),
        )
    if len(regions) > 1:
        return (
            "ordering",
            "{} non-contiguous differing byte regions; layout/ordering differs (first at "
            "offset 0x{:x})".format(len(regions), regions[0][0]),
        )
    return (
        "unknown",
        "single differing byte region at offset 0x{:x} without a recognizable cause".format(
            regions[0][0]
        ),
    )


def compare_artifacts(path_a: Path, path_b: Path) -> Dict[str, object]:
    for path in (path_a, path_b):
        if not path.is_file():
            raise ReproError("artifact not found: {}".format(path))
    data_a = path_a.read_bytes()
    data_b = path_b.read_bytes()
    hash_a = sha256_file(path_a)
    hash_b = sha256_file(path_b)
    identical = data_a == data_b
    result: Dict[str, object] = {
        "identical": identical,
        "artifact_a": {"path": str(path_a), "size": len(data_a), "sha256": hash_a},
        "artifact_b": {"path": str(path_b), "size": len(data_b), "sha256": hash_b},
        "first_difference": None,
        "regions": 0,
        "differing_bytes": 0,
        "classification": "identical" if identical else "unknown",
        "detail": "artifacts are byte-identical" if identical else "",
    }
    if identical:
        return result
    regions = differing_regions(data_a, data_b)
    classification, detail = classify_difference(data_a, data_b, regions)
    first_offset = regions[0][0]
    result.update(
        {
            "first_difference": first_offset,
            "regions": len(regions),
            "differing_bytes": sum(region[2] for region in regions),
            "classification": classification,
            "detail": detail,
            "context_a": printable_context(data_a, first_offset)
            if first_offset < len(data_a)
            else "",
            "context_b": printable_context(data_b, first_offset)
            if first_offset < len(data_b)
            else "",
        }
    )
    return result


def run_command(
    command: Sequence[str],
    label: str,
    timeout: int,
    vcvars: Optional[Path],
    env: Optional[Dict[str, str]] = None,
) -> None:
    info("{}: {}".format(label, " ".join(command)))
    try:
        if vcvars is not None and env is None:
            result = static_runtime.run_in_vcvars(command, vcvars, timeout)
        else:
            result = subprocess.run(
                list(command),
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout,
                env=env,
            )
    except FileNotFoundError:
        raise ReproError("{} not found; install it or pass --cmake PATH".format(command[0]))
    except subprocess.SubprocessError as error:
        raise ReproError("{} failed: {}".format(label, error))
    output = (result.stdout + result.stderr).strip()
    if result.returncode != 0:
        if output:
            print(output, file=sys.stderr)
        if result.returncode in (127, 9009) or "is not recognized" in output:
            raise ReproError(
                "{} not found (exit {}); install it or pass --cmake PATH".format(
                    command[0], result.returncode
                )
            )
        raise ReproError("{} failed (exit {})".format(label, result.returncode))
    if output:
        info(output)


def source_commit(repo_root: Path) -> Optional[str]:
    try:
        result = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "--short=12", "HEAD"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def prepare_clean_source(repo_root: Path, commit: Optional[str]) -> tuple:
    """Check out the pinned commit in a detached worktree; fall back for non-git fixtures."""
    if not commit:
        return repo_root, None
    temporary = Path(tempfile.mkdtemp(prefix="tessera-repro-source-"))
    worktree = temporary / "worktree"
    try:
        result = subprocess.run(
            ["git", "-C", str(repo_root), "worktree", "add", "--detach", str(worktree), commit],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=120,
        )
    except (OSError, subprocess.SubprocessError) as error:
        shutil.rmtree(temporary, ignore_errors=True)
        info("warning: cannot create a clean source worktree ({}); using the caller tree".format(error))
        return repo_root, None
    if result.returncode != 0:
        shutil.rmtree(temporary, ignore_errors=True)
        info(
            "warning: cannot create a clean source worktree at {}; using the caller tree".format(
                commit
            )
        )
        return repo_root, None
    return worktree, temporary


def remove_clean_source(repo_root: Path, worktree: Optional[Path], temporary: Optional[Path]) -> None:
    if worktree is None or temporary is None:
        return
    try:
        subprocess.run(
            ["git", "-C", str(repo_root), "worktree", "remove", "--force", str(worktree)],
            capture_output=True,
            text=True,
            timeout=120,
        )
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def build_once(
    args: argparse.Namespace,
    build_dir: Path,
    vcvars: Optional[Path],
    repo_root: Path,
    commit: Optional[str] = None,
) -> Path:
    if build_dir.exists():
        shutil.rmtree(build_dir)
    configure = [
        args.cmake,
        "-S",
        str(repo_root),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE={}".format(args.config),
        "-DTESSERA_STATIC_RUNTIME=ON",
        "-DTESSERA_BUILD_VULKAN=OFF",
        "-DTESSERA_BUILD_POC=OFF",
        "-DTESSERA_BUILD_TESTS=ON",
        "-DTESSERA_USE_COMPILER_CACHE=OFF",
        "-DTESSERA_WARNINGS_AS_ERRORS=ON",
    ]
    if args.package:
        configure.append("-DTESSERA_PACKAGE_EXECUTABLES={}".format(args.target))
        if commit:
            configure.append("-DTESSERA_PACKAGE_COMMIT={}".format(commit))
    if args.generator:
        configure.extend(["-G", args.generator])
    run_command(configure, "configure", CONFIGURE_TIMEOUT, vcvars)
    run_command(
        [
            args.cmake,
            "--build",
            str(build_dir),
            "--config",
            args.config,
            "--target",
            args.target,
            "--parallel",
        ],
        "build",
        BUILD_TIMEOUT,
        vcvars,
    )
    if args.package:
        run_command(
            [
                args.cmake,
                "--build",
                str(build_dir),
                "--config",
                args.config,
                "--target",
                "tessera-package-stage",
                "--parallel",
            ],
            "package-stage",
            BUILD_TIMEOUT,
            vcvars,
        )
    artifact = static_runtime.find_artifact(build_dir, args.target, args.config)
    if artifact is None or not artifact.is_file():
        raise ReproError(
            "build succeeded but no '{}' artifact was found under {}".format(
                args.target, build_dir
            )
        )
    return artifact


def package_once(
    args: argparse.Namespace, build_dir: Path, archive_env: Dict[str, str]
) -> Path:
    archive_dir = build_dir / "package-out"
    if archive_dir.exists():
        shutil.rmtree(archive_dir)
    generator = "ZIP" if os.name == "nt" else "TGZ"
    cpack = [args.cpack, "--config", str(build_dir / "CPackConfig.cmake"), "-G", generator]
    cpack.extend(["-C", args.config, "-B", str(archive_dir)])
    # Batch wrappers (used by tests and some Windows installs) need cmd; a real cpack is an exe.
    if os.name == "nt" and Path(args.cpack).suffix.lower() in (".bat", ".cmd"):
        cpack = ["cmd", "/c", subprocess.list2cmdline(cpack)]
    run_command(cpack, "cpack", BUILD_TIMEOUT, None, env=archive_env)
    matches = sorted(
        path
        for pattern in ("tessera-*.zip", "tessera-*.tar.gz")
        for path in archive_dir.glob(pattern)
    )
    if not matches:
        raise ReproError("cpack did not produce a tessera archive under {}".format(archive_dir))
    return matches[0]


def print_result(
    result: Dict[str, object], target: str, config: str, package_mode: bool = False
) -> None:
    artifact_a = result["artifact_a"]
    artifact_b = result["artifact_b"]
    assert isinstance(artifact_a, dict) and isinstance(artifact_b, dict)
    print(
        "Reproducible {}: {} ({})".format(
            "package archive" if package_mode else "build", target, config
        )
    )
    print(
        "  A: {} ({} bytes, sha256 {})".format(
            artifact_a["path"], artifact_a["size"], artifact_a["sha256"]
        )
    )
    print(
        "  B: {} ({} bytes, sha256 {})".format(
            artifact_b["path"], artifact_b["size"], artifact_b["sha256"]
        )
    )
    if result["identical"]:
        print("PASS: artifacts are byte-identical.")
        return
    print(
        "FAIL: artifacts differ ({} differing byte(s) in {} region(s), first at offset 0x{:x})".format(
            result["differing_bytes"], result["regions"], result["first_difference"]
        )
    )
    print("  likely cause: {}".format(result["classification"]))
    print("  detail: {}".format(result["detail"]))
    print("  context A: {!r}".format(result.get("context_a", "")))
    print("  context B: {!r}".format(result.get("context_b", "")))


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", choices=("Release", "Debug"), default=DEFAULT_CONFIG)
    parser.add_argument("--target", default=DEFAULT_TARGET)
    parser.add_argument("--build-root", default=str(DEFAULT_BUILD_ROOT))
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--cpack", default="cpack")
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument("--generator", default=None)
    parser.add_argument(
        "--package",
        action="store_true",
        help="compare the CPack archives from the two builds instead of the executables",
    )
    parser.add_argument(
        "--commit",
        default=None,
        help="package commit id pinned into both builds with --package (default: git HEAD)",
    )
    parser.add_argument(
        "--source-date-epoch",
        default=DEFAULT_ARCHIVE_EPOCH,
        help="fixed archive timestamp for --package (default: {})".format(DEFAULT_ARCHIVE_EPOCH),
    )
    parser.add_argument(
        "--archive-timezone",
        default=DEFAULT_ARCHIVE_TZ,
        help="fixed timezone for ZIP DOS timestamps with --package (default: {})".format(
            DEFAULT_ARCHIVE_TZ
        ),
    )
    parser.add_argument(
        "--compare",
        nargs=2,
        metavar=("A", "B"),
        default=None,
        help="compare two existing artifacts instead of building",
    )
    parser.add_argument("--keep", action="store_true", help="keep both build directories")
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)
    global _JSON_MODE
    _JSON_MODE = bool(args.as_json)

    try:
        if args.compare:
            path_a, path_b = (Path(item) for item in args.compare)
            result = compare_artifacts(path_a, path_b)
            target = "{} vs {}".format(path_a.name, path_b.name)
        else:
            repo_root = Path(args.repo_root).resolve()
            build_root = Path(args.build_root).resolve()
            build_a = build_root / "a"
            build_b = build_root / "b"
            vcvars = static_runtime.vcvars_environment()
            if vcvars is not None:
                info("vcvars: {}".format(vcvars))
            commit = (args.commit or source_commit(repo_root)) if args.package else None
            if args.package and not commit:
                raise ReproError(
                    "cannot resolve the git HEAD commit for a reproducible package build"
                )
            if commit:
                info("source commit: {}".format(commit))
            source_root, source_temporary = (
                prepare_clean_source(repo_root, commit) if args.package else (repo_root, None)
            )
            if source_temporary is not None:
                info("source worktree: {}".format(source_root))
            try:
                artifact_a = build_once(args, build_a, vcvars, source_root, commit)
                artifact_b = build_once(args, build_b, vcvars, source_root, commit)
            finally:
                remove_clean_source(repo_root, source_root if source_temporary else None, source_temporary)
            if args.package:
                archive_env = dict(os.environ)
                archive_env.setdefault("SOURCE_DATE_EPOCH", args.source_date_epoch)
                archive_env.setdefault("TZ", args.archive_timezone)
                info(
                    "archive timestamp: SOURCE_DATE_EPOCH={} TZ={}".format(
                        archive_env["SOURCE_DATE_EPOCH"], archive_env["TZ"]
                    )
                )
                artifact_a = package_once(args, build_a, archive_env)
                artifact_b = package_once(args, build_b, archive_env)
                target = "package " + args.target
            else:
                target = args.target
            result = compare_artifacts(artifact_a, artifact_b)
            if not args.keep:
                shutil.rmtree(build_root, ignore_errors=True)
    except ReproError as error:
        print("error: {}".format(error), file=sys.stderr)
        return 2

    if args.as_json:
        payload = dict(result)
        payload.update(
            {
                "ok": bool(result["identical"]),
                "gate": "reproducible-package" if args.package else "reproducible",
                "target": target,
                "mode": "package" if args.package else "binary",
            }
        )
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print_result(result, target, args.config, package_mode=args.package)
    return 0 if result["identical"] else 1


if __name__ == "__main__":
    sys.exit(main())
