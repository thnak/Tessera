#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measurement half of the preview budget gate (issue #170, stage P6).

`check_preview_budget.py` owns the budget file and the two-sided ratchet; this module only produces
numbers, so the policy and the measurement can be read and tested apart.

Four facts about how it measures, because each one is a decision:

**gzip, not brotli, and via the standard library.** The shipped bytes are whatever the CDN serves, and
a browser prefers brotli. Brotli would mean a non-stdlib encoder on the gate's machine (`brotli` is a
third-party Python wheel), which the repository does not depend on, so the budget is recorded in
gzip-9 bytes: deterministic for a fixed input and zlib build, and slightly *larger* than brotli on
this module, so enforcing it cannot hide a real download regression. `encoder()` is stored in the
budget file because a different zlib rebases every size number and that must be a recorded decision,
not a silent drift.

**The page assets are measured from the built `dist/`, not from the sources.** That changed with issue #422
and [ADR-054](../../decisions/ADR-054-studio-front-end-react-vite.md) decision 6: `designer/web` is a Vite +
React project, so the checked-in sources are not what a browser downloads -- the hashed bundle Vite emits into
`dist/` is. The measurement therefore walks `dist/` as a whole (see `page_assets`), which also survives content
hashes: a fixed file list cannot name `assets/index-<hash>.js`, and a name-based list that guessed would either
miss the bundle or measure a stale one.

The rule that replaced the old list is the same rule: **an asset that ships is an asset that is measured**, and
the way to keep that true without a list is to measure the directory. A new public file, a new entry chunk or a
new hashed asset is weighed the moment it is built.

**Latency is the minimum of N fresh processes, normalized to a calibration workload.** Every sample is
a separate `node` process (so JIT state and heap growth from one sample cannot leak into the next), and
the reported figure is the *smallest* of them. Contention on a shared runner can only ever *add* time
-- it cannot make a compile finish sooner than the machine allows -- so the minimum is the closest
estimate of the true cost, while a mean or a median moves with whatever else the runner is doing. This
was measured on a box that also ran a 13-test suite concurrently: one cold sample came out at 122 ms
against a 16 ms baseline, a 7.6x swing that any average would have carried into the budget.

The min alone is not enough to *enforce* (issue #395). The eight Pages-lane runs of 2026-11-16/17
measured `cold_engine_ready_ms` between 16.552 and 28.272 ms and `edit_compile_ms` between 0.083 and
0.223 ms with no relevant source change, so the CI runner alone moves the number 1.7x and 2.7x. A band
tight enough to catch a real regression would therefore fail at random. The fix is the same one the
stylesheet benchmark uses: **ratchet a number that is not machine speed**. Each process also times a
fixed, warmed integer workload, and the reported latency is

    normalized_ms = measured_ms * reference_ms / calibration_ms

where `reference_ms` is the calibration this budget was pinned with (`measurement.calibration_reference_ms`
in the budget file, for the same reason `encoder` is stored: a different value silently rebases every
latency pin). The reference box's speed cancels, so the row still reads in milliseconds and still means
"how long this work takes on the recorded reference profile". Measured on this box: raw
`cold_engine_ready` spread 1.06x across 15 fresh processes (1.08x with 12 CPU-burner processes on a
12-thread machine), normalized spread 1.04x -- the calibration absorbs both CPU-speed and contention
differences, because the loop and the compile run on the same core under the same governor in the same
process.

What this does **not** fix, stated so a reviewer can disagree: it assumes the ratio of integer
arithmetic speed to Wasm-compile speed is roughly constant across x86-64 runner generations, which is
plausible (both are scalar integer code) but unmeasured across hosts -- only one box was measured
here. The `worse` tolerance is set with that unmeasured component in mind, and the first Pages runs
after this lands are the real evidence. The raw wall clock is still reported every run
(`*_raw_ms`) so triage never has to guess which half moved.

**The latency rows are the engine's share only.** Cold time-to-first-frame also needs the browser's
HTML/CSS parse and, from P3, a WebGPU frame; edit-to-view also needs the same frame. Neither exists
in the Node host, so the entries are named for what they measure (`cold_engine_ready_ms`,
`edit_compile_ms`) rather than borrowing the slice's name for a number that is not that slice's
number. Adding `cold_first_frame_ms`/`edit_to_view_ms` beside them is the browser lane's job.

**Every module the studio loads is weighed (issue #395).** ADR-054 decision 3 ships three lazy
`import()`s -- diagnostics, scene preview, canvas -- and the studio loads all three: diagnostics for
the default view, scene when the Scene tab is opened, canvas when the Canvas tab runs. The old
budget weighed diagnostics only, which meant the largest of the three (canvas, 1.29 MB gzip against
diagnostics' 0.38 MB) could grow without anyone noticing. One entry still gates the total, so a
regression fails once rather than three times, but every module's own size is recorded in the
`recorded` breakdown so a report names the file that grew. `MODULE_FILES` is the single list both
halves read: adding a fourth module is one edit here plus one budget entry.
"""

from __future__ import annotations

import json
import os
import subprocess
import zlib
from pathlib import Path
from typing import Dict, List, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

PAGE_DIR = REPO_ROOT / "designer" / "web"
# Where the Vite build puts the page a visitor downloads (issue #422, ADR-054 decision 2). `dist/` is
# git-ignored: it is a build artifact, and the budget is about the artifact, not the sources.
PAGE_DIST = PAGE_DIR / "dist"
SAMPLE = REPO_ROOT / "samples" / "parity-demo" / "Counter.tsx"
PROBE = SCRIPTS_DIR / "ci" / "preview_budget_probe.mjs"

# Every module the studio loads, as a pair of files next to the assembled page (issue #395).
#
# The three are lazy `import()`s from the page's own base (ADR-054 decision 3), never bundled: the
# Pages lane copies each `<name>.js` + `<name>.wasm` pair next to `index.html`, so they are measured
# from the module directory rather than from `dist/`. All three are weighed because the studio loads
# all three -- diagnostics for the default view, scene preview for the Scene tab, canvas for the
# Canvas tab -- and measuring only diagnostics let the biggest of the three (canvas) grow unweighed.
MODULES = ("tessera_diagnostics", "tessera_scene_preview", "tessera_canvas")

# The `.js` loader and the `.wasm` payload for each module, in a stable order so the recorded
# breakdown and a report diff do not reshuffle when a module is added.
MODULE_FILES = tuple(
    "{}.{}".format(module, extension) for module in MODULES for extension in ("js", "wasm")
)

# The built page is measured **as a directory**, not as a list of names.
#
# This replaced `PAGE_FILES` in issue #422. The list could not survive the conversion: Vite emits
# `assets/index-<hash>.js` and `assets/index-<hash>.css`, and a name-based list would have to name a hash
# that changes on every edit. Walking `dist/` also keeps the rule the list existed for -- "a page asset that
# is not weighed ships unweighed" -- true by construction rather than by discipline: a new public file, a new
# entry chunk or a new split chunk is measured the moment it is built.
#
# The three Wasm loaders stay lazy `import()`s and are *not* in the bundle; they are copied into the site by
# `.github/workflows/pages.yml` and are measured from the module directory, all three of them since issue
# #395 (see `MODULES`).
#
# Vite's `public/` directory (fonts, device profiles, skins) is copied verbatim into `dist/`; it is
# measured because it ships. The sample manifest (`samples.json`) used to be one of those files; the Studio
# no longer bundles samples (a visitor opens them from a clone), so it is gone from the walk.
#
# **The lazy mock transpiler is weighed on its own row** (issue #434, ADR-055 decision 5). The studio loads
# the mock host and esbuild-wasm only when a project has a `*.mock.ts` file, and `designer/web/vite.config.ts`
# puts all of it under `dist/assets/mock/`. That directory is left out of `download_gzip_bytes` -- a project
# without mocks never fetches it, so the cold download must not move -- and is weighed by
# `mock_transpiler_lazy_gzip_bytes` instead, so it is neither unweighed nor hiding in the cold number.
PAGE_LAZY_MOCK_DIR = "assets/mock"
PAGE_EXCLUDED_SUFFIXES = (".map",)
PAGE_EXCLUDED_NAMES = (".gitkeep",)

# Where a built module usually is: the Pages lane's assembled site first, then a local wasm configure.
MODULE_CANDIDATES = (
    REPO_ROOT / "build" / "pages",
    REPO_ROOT / "build" / "wasm-webgpu" / "src" / "compiler" / "codegen",
)

# Repetition is part of the measurement's definition, not a tunable: a budget recorded from five
# samples is not the same claim as one recorded from fifty, so the file stores these counts.
COLD_RUNS = 5
EDIT_PROBE_RUNS = 5
EDIT_SAMPLES = 30
EDIT_WARMUP = 5
TIMEOUT_SECONDS = 300

# The probe's calibration workload, mirrored here so the gate can refuse a probe whose report carries
# a different one. The probe is the source of truth for the arithmetic; these are the contract.
#
# `reference_ms` is the calibration measured on the box the current latency pins were recorded on.
# Every pin is divided by it and multiplied by the calibration of the run being checked, so the
# reference box's own speed cancels out of the comparison. Changing it rebases every latency pin,
# which is why it lives in the budget file next to `encoder` rather than in code.
CALIBRATION_ITERATIONS = 4000000
CALIBRATION_CHECKSUM = 40185732

# The two measured entries whose values the gate ratchets, in report order.
SIZE_ENTRIES = ("download_gzip_bytes", "mock_transpiler_lazy_gzip_bytes")
LATENCY_ENTRIES = ("cold_engine_ready_ms", "edit_compile_ms")


class SetupError(Exception):
    """A required input is missing or unreadable, or a subprocess could not run."""


def gzip_size(data: bytes) -> int:
    """gzip-9 bytes, header and trailer included.

    The header carries no file name or timestamp (zlib's wrapper does not add one), so the count
    depends only on the bytes and the zlib build.
    """
    compressor = zlib.compressobj(9, zlib.DEFLATED, 16 + zlib.MAX_WBITS)
    return len(compressor.compress(data) + compressor.flush())


def encoder() -> str:
    """A label for the compressor that produced the recorded sizes."""
    return "python-zlib {}".format(getattr(zlib, "ZLIB_RUNTIME_VERSION", zlib.ZLIB_VERSION))


def require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise SetupError("{} not found: {}".format(label, path))


def default_module_dir() -> Optional[Path]:
    """The first candidate directory that holds **every** module file, or None if there is none.

    Every file, not the first module: a site assembled with diagnostics alone is exactly the state
    issue #395 exists to make impossible to pass, so a partially assembled directory must report as
    missing rather than be measured as a cheaper page.
    """
    for candidate in MODULE_CANDIDATES:
        if all((candidate / name).is_file() for name in MODULE_FILES):
            return candidate
    return None


def default_node() -> str:
    """`EMSDK_NODE` first, then `node` from PATH.

    The WebGPU and Pages lanes already export `EMSDK_NODE` for the parity gates, so the probe runs on
    the same interpreter the module was tested with in CI and on whatever a developer box has.
    """
    return os.environ.get("EMSDK_NODE") or "node"


def _sizes(files: Sequence[Path], label: str, root: Optional[Path] = None) -> Dict[str, int]:
    """The raw and gzip-9 size of each file, keyed by its name relative to `root`.

    `root` is `dist/` for the page, so an entry keeps the layout a reviewer can find on disk
    (`assets/index-<hash>.js`) instead of only its basename -- with hashed names, two assets in different
    directories could otherwise collide on one key.
    """
    sizes: Dict[str, int] = {}
    for path in files:
        require_file(path, label)
        key = path.relative_to(root).as_posix() if root is not None else path.name
        data = path.read_bytes()
        sizes[key] = len(data)
        sizes[key + ".gzip"] = gzip_size(data)
    return sizes


def page_assets(page_dir: Optional[Path] = None) -> List[Path]:
    """Every file the built page ships, walked from `dist/`.

    Sorted, so the recorded breakdown in the budget file is deterministic and a diff names one file rather
    than reshuffling the table. The path is relative to `dist/` with POSIX separators, which is why an entry
    reads `assets/index-<hash>.js` and not a Windows path.

    A missing `dist/` is a `SetupError` at `_sizes`, not an empty measurement: a budget measured over nothing
    would report a huge improvement and pass as "stale", which is the failure mode `require_file` exists to
    prevent.
    """
    root = PAGE_DIST if page_dir is None else page_dir
    assets: List[Path] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        name = path.name
        if name.endswith(PAGE_EXCLUDED_SUFFIXES) or name in PAGE_EXCLUDED_NAMES:
            continue
        if is_lazy_mock(path, root):
            continue
        assets.append(path)
    return assets


def is_lazy_mock(path: Path, root: Path) -> bool:
    """`True` for a file of the lazy mock transpiler chunk (`dist/assets/mock/`)."""
    return path.relative_to(root).as_posix().startswith(PAGE_LAZY_MOCK_DIR + "/")


def lazy_mock_assets(page_dir: Optional[Path] = None) -> List[Path]:
    """The files under `dist/assets/mock/`, sorted; empty when the build has no mock chunk."""
    root = PAGE_DIST if page_dir is None else page_dir
    return [
        path
        for path in sorted(root.rglob("*"))
        if path.is_file()
        and not path.name.endswith(PAGE_EXCLUDED_SUFFIXES)
        and is_lazy_mock(path, root)
    ]


def module_assets(module_dir: Path) -> List[Path]:
    return [module_dir / name for name in MODULE_FILES]


def size_measurements(module_dir: Path) -> Dict[str, float]:
    """The size rows: the compressed download total plus the per-file breakdown.

    The total is the entry the gate ratchets; the breakdown rides along in the budget's `recorded`
    block so a report can name the file that grew without gating six numbers and forcing six edits
    for one change.
    """
    sizes: Dict[str, int] = {}
    sizes.update(_sizes(page_assets(), "page asset", root=PAGE_DIST))
    sizes.update(_sizes(module_assets(module_dir), "Wasm module artifact"))
    sizes["download_gzip_bytes"] = sum(
        value for name, value in sizes.items() if name.endswith(".gzip")
    )
    # Summed, not itemized: the per-file breakdown of `download_gzip_bytes` is the cold path's, and the
    # lazy chunk is one build product (the host chunk and esbuild's WebAssembly) with one number.
    lazy = {
        path.relative_to(PAGE_DIST).as_posix(): gzip_size(path.read_bytes()) for path in lazy_mock_assets()
    }
    sizes["mock_transpiler_lazy_gzip_bytes"] = sum(lazy.values())
    return {name: float(value) for name, value in sizes.items()}


def _run_probe(node: str, module_dir: Path, mode: Sequence[str]) -> Dict:
    require_file(module_dir / MODULE_FILES[0], "Wasm module loader")
    require_file(SAMPLE, "probe source")
    require_file(PROBE, "probe script")
    command = [node, str(PROBE), str(module_dir / MODULE_FILES[0]), str(SAMPLE)]
    command.extend(mode)
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
        raise SetupError("cannot run the preview probe: {}".format(exc)) from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().splitlines()
        raise SetupError(
            "the preview probe failed (exit {}): {}".format(
                result.returncode, detail[0] if detail else "no diagnostics"
            )
        )
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise SetupError("the preview probe did not print JSON: {}".format(exc)) from exc
    if not isinstance(report, dict):
        raise SetupError("the preview probe did not print a JSON object")
    return report


def calibration_from(report: Dict) -> float:
    """The calibration this probe run measured, with its contract enforced.

    A probe whose calibration is missing, zero or the wrong workload is a `SetupError`, not a
    measurement: the normalized latency multiplies by `reference_ms / calibration_ms`, so a
    calibration that silently collapsed (a JIT hoisting the loop, an edit that changed the iteration
    count) would inflate every latency into a false regression, and one that silently changed would
    make the ratio meaningless. The checksum is what makes "it ran in full" checkable rather than
    assumed.
    """
    iterations = report.get("calibration_iterations")
    if iterations != CALIBRATION_ITERATIONS:
        raise SetupError(
            "the preview probe calibrated with {} iterations, expected {}: the measurement's "
            "definition changed, so every latency pin is stale".format(
                iterations, CALIBRATION_ITERATIONS
            )
        )
    checksum = report.get("calibration_checksum")
    if checksum != CALIBRATION_CHECKSUM:
        raise SetupError(
            "the preview probe's calibration checksum is {!r}, expected {}: the workload did not "
            "run as recorded, so the normalized latency would be meaningless".format(
                checksum, CALIBRATION_CHECKSUM
            )
        )
    value = report.get("calibration_ms")
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
        raise SetupError(
            "the preview probe's calibration_ms is {!r}; a non-positive calibration cannot normalize "
            "a latency".format(value)
        )
    return float(value)


def latency_measurements(
    node: str, module_dir: Path, reference_ms: float
) -> Dict[str, float]:
    """The two latency rows plus their raw and calibration companions.

    Each row is `min(measured * reference_ms / calibration)` over fresh processes, in that process's
    own units: multiplying by `reference_ms` and dividing by the process's calibration converts "how
    long this took on this box while it ran" into "how long it takes on the recorded reference
    profile". The minimum is taken *after* normalizing, because what is being minimized is the work,
    not the box: a process that was descheduled has both a larger latency and a larger calibration,
    so the ratio is already mostly immune, and taking the minimum of the ratio picks the cleanest run.

    A minimum, not an average, for the reason in the module docstring: contention only adds time, so
    the smallest sample is the best estimate of the true cost.

    `*_raw_ms` is reported unnormalized, and is not gated: it is the number a human reads to tell
    "the code got slower" from "the box got slower", which is otherwise indistinguishable in the
    normalized row alone.
    """
    cold_raw: List[float] = []
    cold_normalized: List[float] = []
    calibrations: List[float] = []
    for _ in range(COLD_RUNS):
        report = _run_probe(node, module_dir, ("--cold",))
        calibration = calibration_from(report)
        calibrations.append(calibration)
        raw = float(report["cold_engine_ready_raw_ms"])
        cold_raw.append(raw)
        cold_normalized.append(raw * reference_ms / calibration)

    edits_raw: List[float] = []
    edits_normalized: List[float] = []
    for _ in range(EDIT_PROBE_RUNS):
        report = _run_probe(
            node,
            module_dir,
            ("--edits", str(EDIT_SAMPLES), "--warmup", str(EDIT_WARMUP)),
        )
        calibration = calibration_from(report)
        calibrations.append(calibration)
        samples = [float(value) for value in report["samples"]]
        if not samples:
            raise SetupError("the preview probe returned no edit samples")
        # The per-edit minimum *within* a probe is the floor of a single edit; the minimum across
        # probes then picks the quietest process. Both steps remove contention, never the code's cost.
        raw = min(samples)
        edits_raw.append(raw)
        edits_normalized.append(raw * reference_ms / calibration)

    return {
        "cold_engine_ready_ms": round(min(cold_normalized), 3),
        "edit_compile_ms": round(min(edits_normalized), 3),
        # Unnormalized companions, reported but never gated: a human reads these to tell "the code
        # got slower" from "the box got slower", which the normalized row alone cannot distinguish.
        "cold_engine_ready_raw_ms": round(min(cold_raw), 3),
        "edit_compile_raw_ms": round(min(edits_raw), 3),
        # The box's own speed, on the same workload the pins were recorded against. Above the
        # reference means this box is faster than the one the pins came from, and the normalized row
        # is scaled up accordingly; the ratio of the two is the whole normalization, printed so it is
        # auditable from a CI log without re-running anything.
        "calibration_ms": round(min(calibrations), 3),
        "calibration_reference_ms": round(reference_ms, 3),
    }


def measure(
    module_dir: Path, node: str, reference_ms: float, size_only: bool = False
) -> Dict[str, float]:
    """Every measured row. `size_only` skips the probe, which needs a Node runtime."""
    measurements = size_measurements(module_dir)
    if not size_only:
        measurements.update(latency_measurements(node, module_dir, reference_ms))
    return measurements


def measurement_counts(reference_ms: float) -> Dict[str, object]:
    """The sample counts and, when latency was measured, the calibration the rows were recorded with.

    Stored in the budget file, because a budget recorded from five normalized samples on a reference
    calibration is not the same claim as one recorded from fifty against a different reference.

    `reference_ms <= 0` means "this run did not measure latency" (a size-only `--update` on a file
    that has no reference yet). The normalization block is then omitted rather than written as zero:
    zero is not a valid reference, and writing it would turn an untested size-only lane into a budget
    that fails the next full check for a reason that lane did not cause.
    """
    counts: Dict[str, object] = {
        "cold_runs": COLD_RUNS,
        "edit_probe_runs": EDIT_PROBE_RUNS,
        "edit_samples_per_run": EDIT_SAMPLES,
        "edit_warmup": EDIT_WARMUP,
        # The statistic is part of the claim: a budget recorded as a minimum is not the same claim as
        # one recorded as a mean, so the file records which it is.
        "statistic": "min",
    }
    if reference_ms > 0:
        # How a wall-clock number became a portable one (issue #395). `reference_ms` is the divisor
        # the pins were recorded against; `iterations` and `checksum` pin the workload itself, so a
        # probe that quietly stopped doing the same work is a setup error rather than a new baseline.
        counts.update(
            {
                "normalization": "measured_ms * calibration_reference_ms / calibration_ms",
                "calibration_reference_ms": round(reference_ms, 3),
                "calibration_iterations": CALIBRATION_ITERATIONS,
                "calibration_checksum": CALIBRATION_CHECKSUM,
            }
        )
    return counts


def format_rows(measurements: Dict[str, float]) -> List[str]:
    """The report table: deterministic, sorted, one row per measured name."""
    lines: List[str] = []
    for name in sorted(measurements):
        value = measurements[name]
        if float(value).is_integer():
            lines.append("  {:<34} {:>12}".format(name, int(value)))
        else:
            lines.append("  {:<34} {:>12.3f}".format(name, value))
    return lines
