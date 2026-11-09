#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static-link release gate (ADR-012).

Inspects an executable's dynamic dependencies with ``ldd`` (Linux), ``otool -L`` (macOS) or
``dumpbin /dependents`` (Windows), then fails when a third-party dynamic library is outside the
per-OS allowlist or when the C/C++ runtime (``libstdc++``, ``libgcc_s``, ``VCRUNTIME*``,
``MSVCP*``) is linked dynamically. System libraries that cannot be static (the Vulkan loader,
X11/Wayland, Win32, macOS frameworks) are allowlisted per platform.

The script is stdlib-only and deterministic. ``--from-output`` parses synthetic tool output so
the behavior can be tested on any OS; the allowlist can be replaced with ``--allowlist``.

Exit codes: 0 = gate passed, 1 = gate violations, 2 = usage/tool error.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# Dynamic libraries that cannot be static-linked by platform necessity and are therefore
# allowed next to the shipped executable (see decisions/ADR-012-static-linking.md and
# docs/design/rendering-production-plan.md section 6).
DEFAULT_ALLOWLISTS: Dict[str, Tuple[str, ...]] = {
    "linux": (
        "libc.so.*",
        "libm.so.*",
        "libdl.so.*",
        "librt.so.*",
        "libpthread.so.*",
        "libmvec.so.*",
        "linux-vdso.so.*",
        "ld-linux*.so.*",
        "ld64.so.*",
        "libX11.so.*",
        "libX11-xcb.so.*",
        "libxcb.so.*",
        "libxcb-*.so.*",
        "libXau.so.*",
        "libXdmcp.so.*",
        "libwayland-client.so.*",
        "libwayland-cursor.so.*",
        "libwayland-egl.so.*",
        "libxkbcommon.so.*",
        "libxkbcommon-x11.so.*",
        "libvulkan.so.*",
    ),
    "macos": (
        "/usr/lib/libSystem.B.dylib",
        "/usr/lib/libc++.1.dylib",
        "/usr/lib/libc++abi.dylib",
        "/usr/lib/libobjc.A.dylib",
        "/usr/lib/libresolv.9.dylib",
        "/usr/lib/libiconv.2.dylib",
        "/System/Library/Frameworks/*",
    ),
    "windows": (
        "kernel32.dll",
        "user32.dll",
        "gdi32.dll",
        "shell32.dll",
        "advapi32.dll",
        "ole32.dll",
        "vulkan-1.dll",
    ),
}

# Dynamic C/C++ runtimes are always a failure, even when a custom allowlist would match them.
FORBIDDEN_RUNTIME_PATTERNS: Dict[str, Tuple[str, ...]] = {
    "linux": (r"^libstdc\+\+", r"^libgcc_s"),
    "macos": (r"^libstdc\+\+", r"^libgcc_s"),
    "windows": (r"^vcruntime", r"^msvcp", r"^msvcr", r"^ucrtbase", r"^api-ms-win-crt-"),
}

TOOLS = {"linux": "ldd", "macos": "otool", "windows": "dumpbin"}

LDD_RESOLVED = re.compile(r"^\s*(\S+)\s*=>\s*(\S+)\s*\(")
LDD_NOT_FOUND = re.compile(r"^\s*(\S+)\s*=>\s*not found")
LDD_SYSTEM_PATH = re.compile(r"^\s*(/\S+)\s*\(0x")
LDD_PLAIN = re.compile(r"^\s*([\w.+-]+)\s*\(0x")
OTOOL_ENTRY = re.compile(r"^\s*(\S+)\s+\(compatibility version")
DUMPBIN_DLL = re.compile(r"^[A-Za-z0-9_.\-]+\.(?:dll)$", re.IGNORECASE)


@dataclass
class ParsedDependencies:
    libraries: List[Tuple[str, str]]  # (reported name, resolved path or "")
    unresolved: List[str]
    static: bool = False


@dataclass
class Finding:
    name: str
    path: str
    allowed: bool
    reason: str

    def as_dict(self) -> Dict[str, object]:
        return {"name": self.name, "path": self.path, "allowed": self.allowed, "reason": self.reason}


@dataclass
class BinaryReport:
    label: str
    tool: str
    static: bool
    findings: List[Finding]
    violations: List[str]

    def as_dict(self) -> Dict[str, object]:
        return {
            "label": self.label,
            "tool": self.tool,
            "static": self.static,
            "libraries": [finding.as_dict() for finding in self.findings],
            "violations": list(self.violations),
        }


def detect_platform() -> str:
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform == "win32":
        return "windows"
    raise SystemExit(
        "error: unsupported platform {!r}; pass --os linux|macos|windows".format(sys.platform)
    )


def parse_ldd(output: str) -> ParsedDependencies:
    lowered = output.lower()
    if "statically linked" in lowered or "not a dynamic executable" in lowered:
        return ParsedDependencies([], [], static=True)
    libraries: List[Tuple[str, str]] = []
    unresolved: List[str] = []
    for line in output.splitlines():
        match = LDD_NOT_FOUND.match(line)
        if match:
            unresolved.append(match.group(1))
            continue
        match = LDD_RESOLVED.match(line)
        if match:
            libraries.append((match.group(1), match.group(2)))
            continue
        match = LDD_SYSTEM_PATH.match(line)
        if match:
            path = match.group(1)
            libraries.append((os.path.basename(path), path))
            continue
        match = LDD_PLAIN.match(line)
        if match:
            libraries.append((match.group(1), ""))
    return ParsedDependencies(libraries, unresolved)


def parse_otool(output: str) -> ParsedDependencies:
    libraries: List[Tuple[str, str]] = []
    for line in output.splitlines():
        match = OTOOL_ENTRY.match(line)
        if match:
            path = match.group(1)
            libraries.append((os.path.basename(path), path))
    return ParsedDependencies(libraries, [])


def parse_dumpbin(output: str) -> ParsedDependencies:
    libraries: List[Tuple[str, str]] = []
    for line in output.splitlines():
        entry = line.strip()
        if DUMPBIN_DLL.fullmatch(entry):
            libraries.append((entry, entry))
    return ParsedDependencies(libraries, [])


PARSERS = {"ldd": parse_ldd, "otool": parse_otool, "dumpbin": parse_dumpbin}


def msvc_version_key(path: Path) -> Tuple[int, ...]:
    match = re.search(r"(\d+)\.(\d+)\.(\d+)", str(path))
    if not match:
        return (0,)
    return tuple(int(part) for part in match.groups())


def visual_studio_installs() -> List[Path]:
    installs: List[Path] = []
    candidates = [Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))]
    candidates.append(Path(os.environ.get("ProgramFiles", r"C:\Program Files")))
    for base in candidates:
        vswhere = base / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if not vswhere.is_file():
            continue
        try:
            result = subprocess.run(
                [str(vswhere), "-all", "-products", "*", "-property", "installationPath"],
                capture_output=True,
                text=True,
                timeout=60,
            )
        except (OSError, subprocess.SubprocessError):
            continue
        for line in result.stdout.splitlines():
            line = line.strip()
            if line:
                installs.append(Path(line))
    return installs


def find_vcvars64(dumpbin: Optional[Path]) -> Optional[Path]:
    installs = visual_studio_installs()
    if dumpbin is not None:
        # <install>/VC/Tools/MSVC/<version>/bin/Hostx64/x64/dumpbin.exe
        parents = dumpbin.parents
        if len(parents) > 7:
            installs.append(parents[7])
    for install in installs:
        candidate = install / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if candidate.is_file():
            return candidate
    return None


def find_dumpbin(explicit: Optional[str]) -> Optional[Path]:
    if explicit:
        path = Path(explicit)
        return path if path.is_file() else None
    on_path = shutil.which("dumpbin")
    if on_path:
        return Path(on_path)
    candidates: List[Path] = []
    vc_tools = os.environ.get("VCToolsInstallDir")
    if vc_tools:
        candidates.append(Path(vc_tools) / "bin" / "Hostx64" / "x64" / "dumpbin.exe")
    for install in visual_studio_installs():
        candidates.extend(
            (install / "VC" / "Tools" / "MSVC").glob("*/bin/Hostx64/x64/dumpbin.exe")
        )
    existing = [candidate for candidate in candidates if candidate.is_file()]
    if not existing:
        return None
    return max(existing, key=msvc_version_key)


def run_dumpbin(binary: str, dumpbin: Path) -> subprocess.CompletedProcess:
    vcvars = find_vcvars64(dumpbin)
    if vcvars is None:
        return subprocess.run(
            [str(dumpbin), "/dependents", binary], capture_output=True, text=True, timeout=300
        )
    # The MSVC tools need the developer environment (dumpbin loads link.exe support DLLs). A
    # temporary batch file avoids cmd.exe quoting issues for paths containing spaces.
    handle = tempfile.NamedTemporaryFile(
        "w", suffix=".bat", delete=False, encoding="ascii", newline="\r\n"
    )
    try:
        handle.write("@echo off\n")
        handle.write('call "{}" >nul\n'.format(vcvars))
        handle.write('"{}" /dependents "{}"\n'.format(dumpbin, binary))
        handle.write("exit /b %ERRORLEVEL%\n")
        handle.close()
        return subprocess.run(
            ["cmd", "/d", "/c", handle.name], capture_output=True, text=True, timeout=300
        )
    finally:
        try:
            os.unlink(handle.name)
        except OSError:
            pass


def run_tool(os_name: str, binary: str, dumpbin: Optional[str]) -> Tuple[str, str]:
    tool = TOOLS[os_name]
    try:
        if tool == "ldd":
            result = subprocess.run(["ldd", binary], capture_output=True, text=True, timeout=300)
            output = result.stdout + result.stderr
            lowered = output.lower()
            if result.returncode != 0 and not (
                "statically linked" in lowered or "not a dynamic executable" in lowered
            ):
                raise SystemExit(
                    "error: ldd failed on {} (exit {}): {}".format(
                        binary, result.returncode, output.strip()
                    )
                )
            return tool, output
        if tool == "otool":
            result = subprocess.run(
                ["otool", "-L", binary], capture_output=True, text=True, timeout=300
            )
            output = result.stdout + result.stderr
            if result.returncode != 0:
                raise SystemExit(
                    "error: otool failed on {} (exit {}): {}".format(
                        binary, result.returncode, output.strip()
                    )
                )
            return tool, output
        resolved = find_dumpbin(dumpbin)
        if resolved is None:
            raise FileNotFoundError(
                "dumpbin.exe not found; pass --dumpbin PATH or run from a Visual Studio "
                "developer prompt"
            )
        result = run_dumpbin(binary, resolved)
        output = result.stdout + result.stderr
        if result.returncode != 0 or "Dump of file" not in output:
            raise SystemExit(
                "error: dumpbin failed on {} (exit {}): {}".format(
                    binary, result.returncode, output.strip()
                )
            )
        return tool, output
    except FileNotFoundError as error:
        raise SystemExit("error: {} not available: {}".format(tool, error))
    except subprocess.SubprocessError as error:
        raise SystemExit("error: {} failed on {}: {}".format(tool, binary, error))


def load_allowlist(path: Path) -> Tuple[str, ...]:
    patterns: List[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            patterns.append(line)
    return tuple(patterns)


def is_forbidden_runtime(os_name: str, basename: str) -> bool:
    lowered = basename.lower()
    return any(re.match(pattern, lowered) for pattern in FORBIDDEN_RUNTIME_PATTERNS[os_name])


def matches_allowlist(os_name: str, basename: str, path: str, patterns: Sequence[str]) -> bool:
    candidates = [basename, path]
    if os_name == "windows":
        candidates = [candidate.lower() for candidate in candidates]
    for pattern in patterns:
        compare = pattern.lower() if os_name == "windows" else pattern
        for candidate in candidates:
            if candidate and fnmatch.fnmatchcase(candidate, compare):
                return True
    return False


def inspect(
    os_name: str, label: str, tool: str, output: str, patterns: Sequence[str]
) -> BinaryReport:
    parsed = PARSERS[tool](output)
    findings: List[Finding] = []
    violations: List[str] = []

    for name, path in parsed.libraries:
        basename = os.path.basename(path) if path else name
        if is_forbidden_runtime(os_name, basename):
            finding = Finding(
                basename,
                path,
                False,
                "dynamic C/C++ runtime is forbidden; link it statically (ADR-012)",
            )
        elif matches_allowlist(os_name, basename, path, patterns):
            finding = Finding(basename, path, True, "allowlisted system library")
        else:
            finding = Finding(
                basename,
                path,
                False,
                "not in the {} allowlist; statically link this third-party library".format(os_name),
            )
        findings.append(finding)
        if not finding.allowed:
            violations.append("{}: {}".format(basename, finding.reason))

    for name in parsed.unresolved:
        findings.append(Finding(name, "", False, "unresolved dynamic dependency"))
        violations.append("{}: unresolved dynamic dependency".format(name))

    findings.sort(key=lambda item: (item.name.lower(), item.path))
    violations.sort()
    return BinaryReport(label, tool, parsed.static, findings, violations)


def format_report(report: BinaryReport, os_name: str) -> str:
    lines = ["Static-link gate: {} ({}, {})".format(report.label, os_name, report.tool)]
    if report.static:
        lines.append("  allow  (statically linked; no dynamic libraries)")
    for finding in report.findings:
        status = "allow " if finding.allowed else "DENY  "
        if finding.path and finding.path != finding.name:
            lines.append("  {} {} => {}: {}".format(status, finding.name, finding.path, finding.reason))
        else:
            lines.append("  {} {}: {}".format(status, finding.name, finding.reason))
    if report.violations:
        lines.append("FAIL: {} dynamic dependency violation(s).".format(len(report.violations)))
    else:
        lines.append("PASS: all dynamic dependencies are allowlisted.")
    return "\n".join(lines)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--os", choices=("auto", "linux", "macos", "windows"), default="auto")
    parser.add_argument(
        "--binary",
        action="append",
        default=[],
        help="executable to inspect (repeatable)",
    )
    parser.add_argument(
        "--from-output",
        action="append",
        default=[],
        metavar="FILE",
        help="parse a captured ldd/otool/dumpbin output instead of running the tool",
    )
    parser.add_argument(
        "--tool",
        choices=("auto", "ldd", "otool", "dumpbin"),
        default="auto",
        help="parser/tool override (default: derived from --os)",
    )
    parser.add_argument("--dumpbin", default=None, help="explicit dumpbin.exe path (Windows)")
    parser.add_argument(
        "--allowlist",
        default=None,
        metavar="FILE",
        help="replace the built-in allowlist with one glob per line",
    )
    parser.add_argument(
        "--print-allowlist", action="store_true", help="print the effective allowlist and exit"
    )
    parser.add_argument("--json", action="store_true", dest="as_json", help="machine-readable report")
    args = parser.parse_args(argv)

    os_name = detect_platform() if args.os == "auto" else args.os
    tool = TOOLS[os_name] if args.tool == "auto" else args.tool

    patterns = DEFAULT_ALLOWLISTS[os_name]
    if args.allowlist:
        allowlist_path = Path(args.allowlist)
        if not allowlist_path.is_file():
            print("error: allowlist not found: {}".format(allowlist_path), file=sys.stderr)
            return 2
        patterns = load_allowlist(allowlist_path)

    if args.print_allowlist:
        for pattern in patterns:
            print(pattern)
        return 0

    if not args.binary and not args.from_output:
        parser.error("provide --binary PATH or --from-output FILE")

    reports: List[BinaryReport] = []
    for binary in args.binary:
        if not Path(binary).is_file():
            print("error: executable not found: {}".format(binary), file=sys.stderr)
            return 2
        actual_tool, output = run_tool(os_name, binary, args.dumpbin)
        reports.append(inspect(os_name, binary, actual_tool, output, patterns))
    for output_path in args.from_output:
        path = Path(output_path)
        if not path.is_file():
            print("error: tool output not found: {}".format(path), file=sys.stderr)
            return 2
        output = path.read_text(encoding="utf-8", errors="replace")
        reports.append(inspect(os_name, str(path), tool, output, patterns))

    failed = sum(len(report.violations) for report in reports)
    if args.as_json:
        payload = {
            "ok": failed == 0,
            "gate": "static-link",
            "platform": os_name,
            "allowlist": list(patterns),
            "binaries": [report.as_dict() for report in reports],
            "violation_count": failed,
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        for report in reports:
            print(format_report(report, os_name))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
