#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native-versus-Wasm compiler parity gate (issue #164, docs/design/compiler-wasm.md).

Compiles the preview-parity sample with two builds of the *same commit* -- the host
`tessera_codegen_cli` and the Emscripten build of that same target run under Node -- and fails
unless both produce byte-identical output. The accepted artifact is the canonical IR JSON
(`--emit-ir`); by default the run also compares `--mode base --write`, because the generated C++ is
what the downstream preview pipeline actually consumes and it was verified byte-identical when this
gate was added, so asserting it adds drift coverage for the codegen half of the compiler at the cost
of one extra process pair.

The gate builds nothing and downloads nothing: pass it the two CLIs and the emsdk-bundled Node, so
it runs unchanged in CI (see `.github/workflows/webgpu.yml`, job `wasm-parity`) and on a developer
box that already configured both presets.

Exit codes: 0 byte-identical, 1 a byte difference or an unstable repeat, 2 setup failure.

Usage:
  python -B scripts/ci/wasm_ir_parity.py \\
      --native build/ci/src/compiler/codegen/tessera_codegen \\
      --wasm   build/wasm-webgpu/src/compiler/codegen/tessera_codegen.js \\
      --node   "$EMSDK_NODE"

  python -B scripts/ci/wasm_ir_parity.py --ir-only --repeat 3 \\
      --native .../tessera_codegen --wasm .../tessera_codegen.js --node /path/to/node
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SAMPLES = ("samples/parity-demo/Counter.tsx",)
# How many unified-diff lines a mismatch prints. Enough to see the first differing record, short
# enough that a big IR stays readable in a CI log.
DIFF_LINES = 40


class SetupError(Exception):
    """A CLI could not be executed, or an output was not produced."""


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def display_path(path: Path, repo_root: Path) -> str:
    """Repository-relative when possible, else the bare name: keeps the output stable across
    machines and never prints a random temporary directory."""
    try:
        return str(path.resolve().relative_to(repo_root.resolve()))
    except ValueError:
        return path.name


def run(command: Sequence[str], cwd: Path) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            list(command),
            cwd=str(cwd),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as exc:
        raise SetupError("cannot execute {}: {}".format(command[0], exc)) from exc


def require_success(result: subprocess.CompletedProcess, label: str) -> None:
    if result.returncode == 0:
        return
    detail = (result.stderr or result.stdout).strip().splitlines()
    message = detail[0] if detail else "no diagnostics"
    raise SetupError("{} failed (exit {}): {}".format(label, result.returncode, message))


def command_prefixes(native_bin: str, wasm_js: str, node: Optional[str]) -> Tuple[List[str], List[str]]:
    """The two command prefixes: native runs directly, Wasm runs through the emsdk Node."""
    native = [native_bin]
    wasm = [node, wasm_js] if node else [wasm_js]
    return native, wasm


def emit_ir(
    prefix: List[str],
    label: str,
    sample: str,
    destination: Path,
    repo_root: Path,
) -> Path:
    result = run([*prefix, "--emit-ir", str(destination), sample], repo_root)
    require_success(result, "{} --emit-ir".format(label))
    if not destination.is_file():
        raise SetupError("{} exited 0 but wrote no {}".format(label, destination.name))
    return destination


def write_cpp(
    prefix: List[str],
    label: str,
    sample: str,
    out_dir: Path,
    repo_root: Path,
) -> Dict[str, Path]:
    result = run(
        [*prefix, "--mode", "base", "--write", "--out", str(out_dir), sample], repo_root
    )
    require_success(result, "{} --mode base --write".format(label))
    files = sorted(path for path in out_dir.rglob("*") if path.is_file())
    if not files:
        raise SetupError("{} exited 0 but wrote no files under {}".format(label, out_dir.name))
    return {str(path.relative_to(out_dir)): path for path in files}


def read_text(path: Path) -> Optional[List[str]]:
    """Decoded lines for a textual diff, or None when the bytes are not valid UTF-8."""
    try:
        return path.read_text(encoding="utf-8").splitlines(keepends=True)
    except (UnicodeDecodeError, OSError):
        return None


def first_byte_difference(left: Path, right: Path) -> int:
    left_bytes = left.read_bytes()
    right_bytes = right.read_bytes()
    for offset, (a, b) in enumerate(zip(left_bytes, right_bytes)):
        if a != b:
            return offset
    return min(len(left_bytes), len(right_bytes))


def describe_difference(
    label: str,
    native_path: Path,
    wasm_path: Path,
    repo_root: Path,
    limit: int = DIFF_LINES,
) -> List[str]:
    """Actionable mismatch report: sizes, digests, then either a unified diff or the first offset."""
    native_digest = sha256_file(native_path)
    wasm_digest = sha256_file(wasm_path)
    lines = [
        "MISMATCH {}".format(label),
        "  native {}  {} bytes  sha256 {}".format(
            display_path(native_path, repo_root), native_path.stat().st_size, native_digest
        ),
        "  wasm   {}  {} bytes  sha256 {}".format(
            display_path(wasm_path, repo_root), wasm_path.stat().st_size, wasm_digest
        ),
    ]
    native_text = read_text(native_path)
    wasm_text = read_text(wasm_path)
    if native_text is not None and wasm_text is not None:
        diff = list(
            difflib.unified_diff(
                native_text,
                wasm_text,
                fromfile="native/" + label,
                tofile="wasm/" + label,
                n=2,
            )
        )
        if diff:
            lines.append("  unified diff (native -> wasm):")
            lines.extend("    " + line.rstrip("\n") for line in diff[:limit])
            if len(diff) > limit:
                lines.append("    ... {} more diff line(s)".format(len(diff) - limit))
            return lines
    offset = first_byte_difference(native_path, wasm_path)
    lines.append("  first differing byte offset: {}".format(offset))
    return lines


def compare_pair(
    label: str,
    native_path: Path,
    wasm_path: Path,
    repo_root: Path,
) -> Tuple[bool, List[str]]:
    native_digest = sha256_file(native_path)
    wasm_digest = sha256_file(wasm_path)
    if native_digest == wasm_digest and native_path.stat().st_size == wasm_path.stat().st_size:
        return True, [
            "BYTE_IDENTICAL {:<4} {:>6} bytes  sha256 {}".format(
                label, native_path.stat().st_size, native_digest
            )
        ]
    return False, describe_difference(label, native_path, wasm_path, repo_root)


def compare_cpp_trees(
    native_files: Dict[str, Path],
    wasm_files: Dict[str, Path],
    repo_root: Path,
) -> Tuple[bool, List[str]]:
    """Every generated file must exist on both sides with identical bytes; the file *sets* must
    match too, so a silently dropped artifact fails as loudly as changed bytes."""
    lines: List[str] = []
    ok = True
    missing = sorted(set(native_files) - set(wasm_files))
    extra = sorted(set(wasm_files) - set(native_files))
    for name in missing:
        ok = False
        lines.append("MISMATCH cpp: wasm build did not write {}".format(name))
    for name in extra:
        ok = False
        lines.append("MISMATCH cpp: wasm build wrote unexpected {}".format(name))
    for name in sorted(set(native_files) & set(wasm_files)):
        identical, detail = compare_pair("cpp:" + name, native_files[name], wasm_files[name], repo_root)
        if not identical:
            ok = False
            lines.extend(detail)
        else:
            lines.append(
                "BYTE_IDENTICAL cpp  {:>6} bytes  {}  sha256 {}".format(
                    native_files[name].stat().st_size,
                    name,
                    sha256_file(native_files[name]),
                )
            )
    return ok, lines


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, help="host-built tessera_codegen CLI")
    parser.add_argument("--wasm", required=True, help="Emscripten build: tessera_codegen.js")
    parser.add_argument("--node", default=None, help="emsdk Node executable ($EMSDK_NODE)")
    parser.add_argument(
        "--sample",
        action="append",
        default=None,
        help="sample .tsx to compile, relative to --repo-root (default: {})".format(
            DEFAULT_SAMPLES[0]
        ),
    )
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument(
        "--work-dir",
        default=None,
        help="scratch directory for the two output trees (default: <repo-root>/build/wasm-parity, "
        "which is gitignored)",
    )
    parser.add_argument("--ir-only", action="store_true", help="skip the generated-C++ comparison")
    parser.add_argument(
        "--repeat",
        type=int,
        default=1,
        help="run the whole comparison N times and require one digest per side (determinism)",
    )
    args = parser.parse_args(argv)
    if args.repeat < 1:
        parser.error("--repeat must be at least 1")
    return args


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    repo_root = Path(args.repo_root).resolve()
    samples = list(args.sample) if args.sample else list(DEFAULT_SAMPLES)

    for label, path in (("--native", args.native), ("--wasm", args.wasm)):
        if not Path(path).is_file():
            print("SETUP ERROR: {} not found: {}".format(label, path), file=sys.stderr)
            return 2
    if args.node and not Path(args.node).is_file():
        print("SETUP ERROR: --node not found: {}".format(args.node), file=sys.stderr)
        return 2

    native_prefix, wasm_prefix = command_prefixes(args.native, args.wasm, args.node)
    print("native cli: {}".format(args.native))
    print("wasm cli:   {}".format(args.wasm))
    print("node:       {}".format(args.node or "(direct)"))
    print("sample(s):  {}".format(", ".join(samples)))
    print("cpp:        {}".format("skipped (--ir-only)" if args.ir_only else "compared"))
    print("repeat:     {}".format(args.repeat))

    work_root = Path(args.work_dir) if args.work_dir else repo_root / "build" / "wasm-parity"
    # Deliberately not tempfile.mkdtemp(): the scratch tree lives under the gitignored build/ dir
    # so the run is visible where a developer already looks for build output, and so a confined
    # sandbox does not have to grant a second write root.
    if work_root.exists():
        shutil.rmtree(work_root)
    work_root.mkdir(parents=True)
    print("work dir:   {}".format(work_root))
    try:
        digests: Dict[str, List[str]] = {}
        failures: List[str] = []
        for repeat in range(args.repeat):
            if args.repeat > 1:
                print("-- run {}/{} --".format(repeat + 1, args.repeat))
            for sample in samples:
                stem = Path(sample).stem
                run_dir = work_root / "run{}".format(repeat) / stem
                run_dir.mkdir(parents=True, exist_ok=True)

                native_ir = emit_ir(
                    native_prefix, "native", sample, run_dir / "Counter.native.ir.json", repo_root
                )
                wasm_ir = emit_ir(
                    wasm_prefix, "wasm", sample, run_dir / "Counter.wasm.ir.json", repo_root
                )
                identical, detail = compare_pair("ir", native_ir, wasm_ir, repo_root)
                for key, path in (("native", native_ir), ("wasm", wasm_ir)):
                    digests.setdefault(key, []).append(sha256_file(path))
                if not identical:
                    failures.extend(detail)
                    continue
                print("\n".join(detail))

                if not args.ir_only:
                    native_files = write_cpp(
                        native_prefix, "native", sample, run_dir / "native-cpp", repo_root
                    )
                    wasm_files = write_cpp(
                        wasm_prefix, "wasm", sample, run_dir / "wasm-cpp", repo_root
                    )
                    cpp_ok, cpp_detail = compare_cpp_trees(native_files, wasm_files, repo_root)
                    if not cpp_ok:
                        failures.extend(cpp_detail)
                    else:
                        print("\n".join(cpp_detail))

        if failures:
            print("\n".join(failures))
            print("FAILED: native and Wasm output differ")
            return 1

        for side, values in sorted(digests.items()):
            unique = len(set(values))
            print(
                "determinism: {} {} run(s), unique_digests={}".format(
                    side, len(values), unique
                )
            )
            if unique != 1:
                print("FAILED: {} output is not deterministic across runs".format(side))
                return 1
        print("PARITY OK: native and Wasm compiler output is byte-identical")
        return 0
    except SetupError as exc:
        print("SETUP ERROR: {}".format(exc), file=sys.stderr)
        return 2
    finally:
        shutil.rmtree(work_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
