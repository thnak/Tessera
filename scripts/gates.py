#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""One-command release-gate runner.

Runs the repository gates in a documented order, streams each gate's output, and prints one summary
table (gate | status | duration). Exit codes: 0 all gates passed, 1 at least one gate failed,
2 setup failure (for example a gate script is missing or an option is invalid).

Gate order:
  1. format-version  clang-format matches the version pinned in tools/format-version.txt
  2. dependencies    license allowlist, statuses, notices, duplicate rows
  3. license-bundle  THIRD_PARTY_LICENSES.txt is up to date (adds --check)
  4. sbom            sbom.spdx.json is up to date (adds --check)
  5. docs            relative links/anchors, docs index, fences, sizes, ADRs
  6. pins            reproducible dependency pins + dependencies.md rows (adds --check)
  7. package         only with --archive PATH: validates the distributable archive
  8. fixtures        only with --fixtures: the gate self-tests (unittest discovery)

The runner is cwd-independent (children run from the repository root) and forces UTF-8 for child
processes while making its own stdout/stderr tolerant, so it behaves the same on Windows cp1252
consoles as on Linux/macOS.

Usage:
  python -B scripts/gates.py
  python -B scripts/gates.py --only docs,pins
  python -B scripts/gates.py --archive build/package/out/tessera-0.0.1-linux-x86_64.tar.gz
  python -B scripts/gates.py --json --quiet
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent


@dataclass(frozen=True)
class Gate:
    name: str
    script: str
    args: tuple
    description: str


BASE_GATES: List[Gate] = [
    Gate(
        "format-version",
        "check_format_version.py",
        (),
        "clang-format matches the version pinned in tools/format-version.txt",
    ),
    Gate("dependencies", "check_dependencies.py", (), "license allowlist, statuses and notices"),
    Gate("license-bundle", "gen_license_bundle.py", ("--check",), "license bundle is up to date"),
    Gate("sbom", "gen_sbom.py", ("--check",), "SPDX 2.3 SBOM is up to date"),
    Gate("docs", "check_docs.py", (), "links, anchors, docs index, fences, sizes, ADRs"),
    Gate("pins", "check_pins.py", ("--check",), "reproducible pins and dependency rows"),
]
OPTIONAL_GATES: List[Gate] = [
    Gate("package", "check_package.py", (), "distributable archive contents (needs --archive)"),
    Gate("fixtures", "-", (), "gate self-tests (unittest discovery, needs --fixtures)"),
]


@dataclass
class GateResult:
    name: str
    status: str  # "pass" | "fail" | "setup-error"
    exit_code: int
    duration_seconds: float
    output: str = ""


def configure_streams() -> None:
    """Keep Unicode output from crashing on legacy (cp1252) consoles."""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(errors="backslashreplace")
            except (ValueError, OSError):
                pass


def child_environment() -> Dict[str, str]:
    environment = dict(os.environ)
    # Child gates print repository text; force UTF-8 so a cp1252 console cannot raise
    # UnicodeEncodeError inside a gate. The encoding of the parent's summary is handled above.
    environment["PYTHONIOENCODING"] = "utf-8"
    environment["PYTHONUTF8"] = "1"
    return environment


def gate_command(gate: Gate, repo_root: Path) -> List[str]:
    if gate.name == "fixtures":
        return [
            sys.executable,
            "-B",
            "-m",
            "unittest",
            "discover",
            "-s",
            str(repo_root / "scripts" / "tests"),
            "-p",
            "test_*.py",
            "-q",
        ]
    return [
        sys.executable,
        "-B",
        str(repo_root / "scripts" / gate.script),
        *gate.args,
    ]


def run_gate(gate: Gate, repo_root: Path, quiet: bool, capture: bool) -> GateResult:
    command = gate_command(gate, repo_root)
    script_path = repo_root / "scripts" / gate.script
    if gate.name != "fixtures" and not script_path.is_file():
        message = "gate script not found: {}".format(script_path)
        print("SETUP ERROR {}: {}".format(gate.name, message), file=sys.stderr)
        return GateResult(gate.name, "setup-error", 2, 0.0, message)

    if not quiet and not capture:
        print("== gate: {} ==".format(gate.name))
    started = time.perf_counter()
    try:
        result = subprocess.run(
            command,
            cwd=str(repo_root),
            env=child_environment(),
            text=True,
            encoding="utf-8",
            errors="replace",
            capture_output=capture or quiet,
        )
    except FileNotFoundError:
        duration = time.perf_counter() - started
        message = "could not start gate command: {}".format(command[0])
        print("SETUP ERROR {}: {}".format(gate.name, message), file=sys.stderr)
        return GateResult(gate.name, "setup-error", 2, duration, message)
    duration = time.perf_counter() - started
    output = ""
    if capture or quiet:
        output = (result.stdout or "") + (result.stderr or "")
        if not quiet and output:
            print(output, end="" if output.endswith("\n") else "\n")
    status = "pass" if result.returncode == 0 else "fail"
    return GateResult(gate.name, status, result.returncode, duration, output)


def select_gates(args: argparse.Namespace) -> List[Gate]:
    gates = list(BASE_GATES)
    if args.archive:
        gates.append(
            Gate("package", "check_package.py", ("--archive", args.archive), OPTIONAL_GATES[0].description)
        )
    if args.fixtures:
        gates.append(OPTIONAL_GATES[1])
    if not args.only:
        return gates
    requested = [name.strip() for name in args.only.split(",") if name.strip()]
    by_name = {gate.name: gate for gate in gates}
    unknown = [name for name in requested if name not in by_name]
    if unknown:
        raise SystemExit("error: unknown gate(s): {}".format(", ".join(sorted(unknown))))
    return [by_name[name] for name in requested]


def print_list() -> None:
    print("Release gates (in order):")
    for gate in BASE_GATES:
        print("  {:<15} {}".format(gate.name, gate.description))
    for gate in OPTIONAL_GATES:
        print("  {:<15} {}".format(gate.name, gate.description))


def summary_table(results: Sequence[GateResult]) -> str:
    lines = ["Gate summary", "------------"]
    lines.append("{:<15} {:<12} {:>8}".format("gate", "status", "duration"))
    for result in results:
        lines.append(
            "{:<15} {:<12} {:>8.2f}s".format(
                result.name, result.status.upper(), result.duration_seconds
            )
        )
    passed = sum(1 for result in results if result.status == "pass")
    failed = sum(1 for result in results if result.status == "fail")
    setup_errors = sum(1 for result in results if result.status == "setup-error")
    lines.append(
        "{} gate(s): {} passed, {} failed, {} setup error(s)".format(
            len(results), passed, failed, setup_errors
        )
    )
    return "\n".join(lines)


def main(argv: Optional[Sequence[str]] = None) -> int:
    configure_streams()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--repo-root",
        default=str(REPO_ROOT),
        help="repository root containing scripts/ (default: the checkout with this script)",
    )
    parser.add_argument("--only", default=None, metavar="NAMES", help="comma-separated gate names")
    parser.add_argument("--list", action="store_true", dest="list_gates", help="list gates and exit")
    parser.add_argument("--archive", default=None, help="run the package gate on this archive")
    parser.add_argument(
        "--fixtures", action="store_true", help="also run the gate self-tests (slow)"
    )
    parser.add_argument("--json", action="store_true", dest="as_json", help="machine-readable report")
    parser.add_argument("--quiet", action="store_true", help="summary only, no gate output")
    args = parser.parse_args(argv)

    repo_root = Path(args.repo_root).resolve()
    if not (repo_root / "scripts").is_dir():
        print("error: no scripts/ directory under {}".format(repo_root), file=sys.stderr)
        return 2

    if args.list_gates:
        print_list()
        return 0

    try:
        gates = select_gates(args)
    except SystemExit as error:
        print(error, file=sys.stderr)
        return 2

    capture = args.as_json or args.quiet
    results: List[GateResult] = []
    for gate in gates:
        results.append(run_gate(gate, repo_root, args.quiet, capture))

    failed = any(result.status == "fail" for result in results)
    setup_errors = any(result.status == "setup-error" for result in results)
    exit_code = 2 if setup_errors else 1 if failed else 0

    if args.as_json:
        payload = {
            "ok": exit_code == 0,
            "gate": "aggregator",
            "gates": [
                {
                    "name": result.name,
                    "status": result.status,
                    "exit_code": result.exit_code,
                    "duration_seconds": round(result.duration_seconds, 3),
                    "output": result.output,
                }
                for result in results
            ],
            "exit_code": exit_code,
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
    elif not args.quiet:
        print("")
        print(summary_table(results))
    else:
        print(summary_table(results))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
