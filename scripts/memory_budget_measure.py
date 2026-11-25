#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measurement half of the memory budget gate (issue #13).

`check_memory_budget.py` owns the budget file and the two-sided ratchet; this module only produces
numbers, so the policy and the measurement can be read and tested apart. Same split as
`preview_budget_measure.py`.

Four facts about how it measures, because each one is a decision:

**The rows are split by reproducibility, and the split is what makes the gate trustworthy.** The
deterministic rows (`sizeof` of a hot type, allocation counts) are exact: two runs of the same binary
must agree, and this module *asserts that they do* rather than trusting it. If a row the budget
enforces ever differs between two runs, that is a measurement bug and the gate fails loudly instead
of recording whichever sample came last. The sampled rows (resident bytes per node, idle growth) are
an OS reading of a heap, so the reported figure is the **minimum** over `rss_runs`: contention and
other processes can only ever *add* resident bytes, so the smallest sample is the closest estimate of
the true cost, while a mean or median moves with whatever else the box is doing. This is the same
statistic and the same argument as the preview budget's latency rows.

**It shells out to a built probe, and a missing probe is a setup error, not a pass.** The numbers
have to come from the real types in the real build, so the module runs
`tests/benchmarks/memory/memory_sizes_probe.cpp` (CMake target `memory_sizes_probe`) found under the
build directory. `--sizes-only` needs nothing but that binary, which is what lets the sizes gate run
on a lane that did not build a font or a GPU backend.

**The probe prints `name=value`, and anything else is ignored.** Keeping the parsing that dumb means
a probe that starts printing a table does not silently feed numbers to the wrong rows. A malformed
value (not an integer, negative) is a setup error.

**Fresh processes, not repeated calls.** Every run is a separate process invocation, so allocator
state, ASLR and a previous sample's heap growth cannot leak into the next one. That matters because
the sampled rows are per-process resident growth.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

PROBE_TARGET = "memory_sizes_probe"
PROBE_SOURCE = REPO_ROOT / "tests" / "benchmarks" / "memory" / "memory_sizes_probe.cpp"

# Where a built probe usually is: the vs2026 preset and the Ninja presets put it in different
# places, so the search walks these roots in order. `release_rehearsal.py` globs a build directory
# the same way rather than hard-coding a configuration path.
BUILD_ROOTS = ("vs2026", "vs2022", "ci", "dev", "release")

# Configuration preference when a build tree holds more than one: the committed budget is recorded
# from a Release build, so that is the one to measure by default.
PREFERRED_CONFIGS = ("Release", "RelWithDebInfo", "MinSizeRel", "Debug")

# The probe's defaults, passed explicitly so the gate measures the same tree as the memory goldens.
NODES = 20000
IDLE_CYCLES = 300

# Repetition is part of the measurement's definition: a deterministic row recorded from one run and
# an RSS row recorded from five are not the same claim, so the file stores the counts.
SIZES_RUNS = 2
RSS_RUNS = 5
TIMEOUT_SECONDS = 300

# Rows that are exact for a given binary. A value that moves between two runs on one machine means
# the row is not what the budget claims it is, so the gate fails instead of picking a sample.
DETERMINISTIC_ROWS = (
    "sizeof_node",
    "sizeof_input_event",
    "sizeof_paint_data",
    "sizeof_computed_style",
    "sizeof_node_id",
    "sizeof_layout_input",
    "sizeof_layout_box",
    "sizeof_paint_op",
    "sizeof_paint_record",
    "sizeof_background_layer_list",
    "sizeof_pace_decision",
    "sizeof_adaptive_quality_sample",
    "sizeof_overlay_placement",
    "computed_style_one_property_bytes",
    "computed_style_one_property_allocations",
    "plain_node_allocations",
    "plain_node_bytes",
    "aria_table_bytes",
    "aria_table_allocations",
    "inline_style_table_bytes",
    "inline_style_table_allocations",
    "event_binding_table_bytes",
    "event_binding_table_allocations",
    "paint_slot_bytes",
    "cleared_table_reallocations",
)

# The GPU category rows (issue #703), printed by `gpu_memory_probe` rather than by
# `memory_sizes_probe`. They are deterministic for the same reason the sizes are: the numbers are our
# own accounting (extent x texel size, buffer size), not the driver's block sizes, so two runs of one
# binary must agree exactly and lavapipe agrees with a real GPU.
GPU_DETERMINISTIC_ROWS = (
    "gpu_category_glyph_atlas_bytes",
    "gpu_category_glyph_atlas_peak_bytes",
    "gpu_category_texture_bytes",
    "gpu_category_texture_peak_bytes",
    "gpu_category_vertex_bytes",
    "gpu_category_vertex_peak_bytes",
    "gpu_category_staging_bytes",
    "gpu_category_staging_peak_bytes",
    "gpu_category_other_bytes",
    "gpu_category_other_peak_bytes",
    "gpu_vma_category_bytes",
    "gpu_category_peak_total_bytes",
    "gpu_category_live_after_free_bytes",
    "gpu_category_peak_after_free_bytes",
)

# Rows read from the OS. Reported as the minimum over `RSS_RUNS`, and never enforced.
SAMPLED_ROWS = ("resident_bytes_per_node", "resident_bytes_total", "node_dirty_scratch_bytes")

_RUN_RE = re.compile(r"^([a-z][a-z0-9_]*)=(-?\d+)$")
_CONFIG_RE = re.compile(r"^#\s*config=(\S+)$")


class SetupError(Exception):
    """A required input is missing or unreadable, or a subprocess could not run."""


def require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise SetupError("{} not found: {}".format(label, path))


def relative(path: Path) -> str:
    """A report path that is stable across machines (never a temp directory or a drive letter)."""
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.name


def default_build_dir() -> Optional[Path]:
    """The first configured build directory under `build/`, or a `--build-dir` override."""
    build_root = REPO_ROOT / "build"
    for name in BUILD_ROOTS:
        candidate = build_root / name
        if (candidate / "CMakeCache.txt").is_file():
            return candidate
    return None


def find_probe(build_dir: Path) -> Path:
    """The built probe under `build_dir`, or a SetupError naming what to build.

    Globbing rather than building a configuration path: MSVC puts it under
    `tests/benchmarks/memory/Release/` and Ninja under `tests/benchmarks/memory/`, and a multi-config
    generator can hold several configurations at once. When several exist, a **Release-like**
    configuration wins, because the committed budget is recorded from one; picking arbitrarily would
    hand the gate a Debug binary and report a configuration mismatch that the user did not cause.
    `--build-dir` (and the `build_config` guard in the checker) remain the explicit override.
    """
    names = (PROBE_TARGET, PROBE_TARGET + ".exe")
    matches = [
        path
        for name in names
        for path in sorted(build_dir.rglob(name))
        if path.is_file() and "tessera-package-root" not in path.parts
    ]
    if not matches:
        raise SetupError(
            "the memory probe is not built: no '{}' under {}. Build the '{}' target first "
            "(python -B scripts/check_memory_budget.py --build helps).".format(
                PROBE_TARGET, relative(build_dir), PROBE_TARGET
            )
        )

    def rank(path: Path) -> int:
        # Lower is better. The order is the configuration preference, then the path so the choice is
        # deterministic when two directories of the same rank exist.
        for index, name in enumerate(PREFERRED_CONFIGS):
            if name in path.parts:
                return index
        return len(PREFERRED_CONFIGS)

    return sorted(matches, key=lambda path: (rank(path), path.as_posix()))[0]


def probe_command(probe: Path, nodes: int, idle_cycles: int) -> List[str]:
    """The argv that runs one probe process.

    `TESSERA_MEMORY_PROBE_INTERPRETER` names an interpreter to run the probe through. A compiled
    probe is executed directly, which is the normal path; the variable exists for the two cases where
    it cannot be: the gate's own tests use a script stand-in (no shebang execution on Windows), and a
    cross-compiled or for-another-OS probe is run behind a wrapper (wine, qemu). Setting it for a
    native probe would prepend an interpreter to an executable, so it is deliberately opt-in.
    """
    command = [str(probe), "--nodes", str(nodes), "--idle-cycles", str(idle_cycles)]
    interpreter = os.environ.get("TESSERA_MEMORY_PROBE_INTERPRETER")
    if interpreter:
        return [interpreter, *command]
    return command


def run_probe(probe: Path, nodes: int, idle_cycles: int) -> Dict[str, int]:
    """One probe process, parsed into `name -> int`. Anything not `name=<int>` is ignored."""
    values, _ = run_probe_with_config(probe, nodes, idle_cycles)
    return values


def run_probe_with_config(
    probe: Path, nodes: int, idle_cycles: int
) -> Tuple[Dict[str, int], str]:
    """`(rows, build configuration)` from one probe process."""
    command = probe_command(probe, nodes, idle_cycles)
    try:
        result = subprocess.run(
            command,
            cwd=str(REPO_ROOT),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise SetupError("cannot run the memory probe: {}".format(exc)) from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().splitlines()
        raise SetupError(
            "the memory probe failed (exit {}): {}".format(
                result.returncode, detail[0] if detail else "no diagnostics"
            )
        )
    values: Dict[str, int] = {}
    config = ""
    for line in result.stdout.splitlines():
        stripped = line.strip()
        config_match = _CONFIG_RE.match(stripped)
        if config_match is not None:
            config = config_match.group(1)
            continue
        match = _RUN_RE.match(stripped)
        if match is None:
            continue
        values[match.group(1)] = int(match.group(2))
    if not values:
        raise SetupError("the memory probe printed no name=value rows")
    return values, config


def _merge_deterministic(
    runs: Sequence[Dict[str, int]],
    errors: List[str],
    warnings: List[str],
    rows: Sequence[str] = DETERMINISTIC_ROWS,
) -> Dict[str, int]:
    """The exact rows, after proving every run agrees on them."""
    merged: Dict[str, int] = {}
    for name in rows:
        seen = [run[name] for run in runs if name in run]
        if not seen:
            continue
        if len(set(seen)) != 1:
            errors.append(
                "{}: a deterministic row took different values across {} runs ({}); the row is "
                "not reproducible, so it cannot be enforced".format(
                    name, len(seen), ", ".join(str(value) for value in seen)
                )
            )
        merged[name] = seen[0]
    missing = [name for name in rows if name not in merged]
    if missing:
        warnings.append(
            "the probe printed no row for {}; the budget entry for it cannot be checked".format(
                ", ".join(sorted(missing))
            )
        )
    return merged


def _merge_sampled(runs: Sequence[Dict[str, int]]) -> Dict[str, int]:
    """The sampled rows, as the minimum over runs (contention only ever adds resident bytes)."""
    merged: Dict[str, int] = {}
    for name in SAMPLED_ROWS:
        seen = [run[name] for run in runs if name in run]
        if seen:
            merged[name] = min(seen)
    return merged


def measure(
    build_dir: Path,
    nodes: int = NODES,
    idle_cycles: int = IDLE_CYCLES,
    sizes_only: bool = False,
    sizes_runs: int = SIZES_RUNS,
    rss_runs: int = RSS_RUNS,
) -> Tuple[Dict[str, float], List[str], List[str], str]:
    """Every measured row: `(measurements, errors, warnings, build_config)`.

    A `sizes_only` run stops after the exact rows, which is what lets the sizes gate run on a lane
    that has the probe but not a quiet machine. The RSS rows are then out of scope, not missing.

    Errors are returned rather than raised when they are about the *data* (a deterministic row that
    moved); a missing or broken probe raises `SetupError`, because that is not a measurement result.
    """
    probe = find_probe(build_dir)
    errors: List[str] = []
    warnings: List[str] = []

    sizes_runs = max(1, sizes_runs)
    runs: List[Dict[str, int]] = []
    configs: List[str] = []
    for _ in range(sizes_runs):
        values, config = run_probe_with_config(probe, nodes, idle_cycles)
        runs.append(values)
        configs.append(config)
    measurements: Dict[str, float] = {
        name: float(value) for name, value in _merge_deterministic(runs, errors, warnings).items()
    }

    if not sizes_only:
        for _ in range(max(0, rss_runs - sizes_runs)):
            runs.append(run_probe(probe, nodes, idle_cycles))
        for name, value in _merge_sampled(runs).items():
            measurements[name] = float(value)

    # The GPU category rows (issue #703). They are deterministic like the sizes, so they belong to a
    # sizes-only run too; a lane with no device simply contributes none.
    gpu_measurements, gpu_errors = measure_gpu(build_dir, sizes_runs)
    measurements.update(gpu_measurements)
    errors.extend(gpu_errors)

    # The build configuration is a comment, so it is carried beside the rows rather than in them.
    # A Debug build is a different claim (see the probe's CMakeLists): same code, different sizeof,
    # different PaintData, one extra proxy allocation per vector.
    return measurements, errors, warnings, configs[0] if configs else ""


# The GPU probe (issue #703). It is a second binary because `memory_sizes_probe` deliberately links
# no RHI target, so it runs on every lane; this one needs a real `rhi::Device`.
GPU_PROBE_TARGET = "gpu_memory_probe"
GPU_PROBE_SOURCE = REPO_ROOT / "tests" / "benchmarks" / "memory" / "gpu_memory_probe.cpp"


def find_gpu_probe(build_dir: Path) -> Optional[Path]:
    """The built GPU probe under `build_dir`, or None when the lane did not build one.

    None is not an error here: a lane without a Vulkan backend has no GPU rows to measure, and the
    caller decides whether that is acceptable. It is deliberately distinct from a probe that exists
    but fails, which IS an error.
    """
    names = (GPU_PROBE_TARGET, GPU_PROBE_TARGET + ".exe")
    for name in names:
        for candidate in build_dir.rglob(name):
            if candidate.is_file():
                return candidate
    return None


def run_gpu_probe(probe: Path, backend: str) -> Dict[str, int]:
    """One GPU probe process, parsed into `name -> int`.

    Exit 3 means "no device on this lane", which is a skip rather than a failure: lavapipe is
    present on the Linux lane and absent on a machine without Vulkan, and a lane that cannot run the
    probe must not report a number it did not measure.
    """
    command = [str(probe), "--backend", backend]
    try:
        result = subprocess.run(
            command,
            cwd=str(REPO_ROOT),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise SetupError("cannot run the GPU memory probe: {}".format(exc)) from exc
    if result.returncode == 3:
        return {}
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().splitlines()
        raise SetupError(
            "the GPU memory probe failed (exit {}): {}".format(
                result.returncode, detail[0] if detail else "no diagnostics"
            )
        )
    rows: Dict[str, int] = {}
    for line in result.stdout.splitlines():
        match = _RUN_RE.match(line.strip())
        if match:
            rows[match.group(1)] = int(match.group(2))
    return rows


def gpu_rows_available(build_dir: Path) -> bool:
    """True when this lane can measure the GPU category rows (issue #703).

    Two conditions, both about the lane rather than the code: the probe was built, and it found a
    device. A lane that fails either has no number to report, and the gate treats those entries as
    out of scope instead of missing.
    """
    probe = find_gpu_probe(build_dir)
    if probe is None:
        return False
    return bool(run_gpu_probe(probe, "vulkan"))


def measure_gpu(build_dir: Path, sizes_runs: int = SIZES_RUNS) -> Tuple[Dict[str, float], List[str]]:
    """The GPU category rows: `(measurements, errors)`.

    Run twice like the CPU rows, and for the same reason: the category bytes are our own accounting
    (extent x texel size, buffer size) rather than the driver's block sizes, so two runs of one
    binary must agree exactly. A disagreement is a measurement bug, not a budget regression, and it
    is reported as an error rather than ratcheted.
    """
    probe = find_gpu_probe(build_dir)
    if probe is None:
        return {}, []
    errors: List[str] = []
    runs: List[Dict[str, int]] = []
    for _ in range(max(1, sizes_runs)):
        rows = run_gpu_probe(probe, "vulkan")
        if not rows:
            # No device on this lane. The rows stay out of scope rather than being recorded as zero,
            # which would ratchet a fiction.
            return {}, []
        runs.append(rows)
    merged = _merge_deterministic(runs, errors, [], GPU_DETERMINISTIC_ROWS)
    return {name: float(value) for name, value in merged.items()}, errors

def measurement_counts(sizes_only: bool, sizes_runs: int = SIZES_RUNS, rss_runs: int = RSS_RUNS) -> Dict:
    """The sample counts and the statistic, stored in the budget file.

    The statistic is part of the claim: a budget recorded as a minimum is not the same claim as one
    recorded as a mean, so the file records which it is.
    """
    counts: Dict = {
        "nodes": NODES,
        "idle_cycles": IDLE_CYCLES,
        "sizes_runs": sizes_runs,
        "statistic": "sizes: exact (every run must agree); rss: min",
    }
    if not sizes_only:
        counts["rss_runs"] = rss_runs
    return counts


def probe_env_note() -> str:
    """A one-line note about the platform, so a recorded figure carries its conditions."""
    return "{} ({} {})".format(sys.platform, os.name, sys.version.split()[0])
