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

**Latency is the minimum of N fresh processes.** Every sample is a separate `node` process (so JIT
state and heap growth from one sample cannot leak into the next), and the reported figure is the
*smallest* of them. Contention on a shared runner can only ever *add* time -- it cannot make a compile
finish sooner than the machine allows -- so the minimum is the closest estimate of the true cost,
while a mean or a median moves with whatever else the runner is doing. This was measured on a box that
also ran a 13-test suite concurrently: one cold sample came out at 122 ms against a 16 ms baseline, a
7.6x swing that any average would have carried into the budget. The minimum discards that sample,
which is exactly right for a number that only ever warns.

**The latency rows are the engine's share only.** Cold time-to-first-frame also needs the browser's
HTML/CSS parse and, from P3, a WebGPU frame; edit-to-view also needs the same frame. Neither exists
in the Node host, so the entries are named for what they measure (`cold_engine_ready_ms`,
`edit_compile_ms`) rather than borrowing the slice's name for a number that is not that slice's
number. Adding `cold_first_frame_ms`/`edit_to_view_ms` beside them is the browser lane's job.
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

# The Wasm module's two files are copied *next to* the built page by the Pages lane, so they are measured
# from the module directory rather than from `dist/`.
MODULE_FILES = ("tessera_diagnostics.js", "tessera_diagnostics.wasm")

# The built page is measured **as a directory**, not as a list of names.
#
# This replaced `PAGE_FILES` in issue #422. The list could not survive the conversion: Vite emits
# `assets/index-<hash>.js` and `assets/index-<hash>.css`, and a name-based list would have to name a hash
# that changes on every edit. Walking `dist/` also keeps the rule the list existed for -- "a page asset that
# is not weighed ships unweighed" -- true by construction rather than by discipline: a new public file, a new
# entry chunk or a new split chunk is measured the moment it is built.
#
# The three Wasm loaders stay lazy `import()`s and are *not* in the bundle; they are copied into the site by
# `.github/workflows/pages.yml` and are measured only as `tessera_diagnostics`, which is the one on the
# first-paint path (the other two are the visitor's choice, exactly as before -- issue #395 is where counting
# them is decided).
#
# `scene.html`, `canvas.html`, `redirect.js` and `samples.json` are Vite's `public/` directory, copied
# verbatim into `dist/`; they are measured because they ship.
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

# The two measured entries whose values the gate ratchets, in report order.
SIZE_ENTRIES = ("download_gzip_bytes",)
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
    """The first candidate directory that holds both module files, or None if there is none."""
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
        assets.append(path)
    return assets


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


def latency_measurements(node: str, module_dir: Path) -> Dict[str, float]:
    """The two latency rows, each the minimum over fresh processes.

    A minimum, not an average, for the reason in the module docstring: contention only adds time, so
    the smallest sample is the best estimate of the true cost and the statistic least likely to fire a
    warning for a reason that has nothing to do with the code.
    """
    cold: List[float] = []
    for _ in range(COLD_RUNS):
        cold.append(float(_run_probe(node, module_dir, ("--cold",))["cold_engine_ready_ms"]))

    edits: List[float] = []
    for _ in range(EDIT_PROBE_RUNS):
        report = _run_probe(
            node,
            module_dir,
            ("--edits", str(EDIT_SAMPLES), "--warmup", str(EDIT_WARMUP)),
        )
        samples = [float(value) for value in report["samples"]]
        if not samples:
            raise SetupError("the preview probe returned no edit samples")
        # The per-edit minimum *within* a probe is the floor of a single edit; the minimum across
        # probes then picks the quietest process. Both steps remove contention, never the code's cost.
        edits.append(min(samples))

    return {
        "cold_engine_ready_ms": round(min(cold), 3),
        "edit_compile_ms": round(min(edits), 3),
    }


def measure(module_dir: Path, node: str, size_only: bool = False) -> Dict[str, float]:
    """Every measured row. `size_only` skips the probe, which needs a Node runtime."""
    measurements = size_measurements(module_dir)
    if not size_only:
        measurements.update(latency_measurements(node, module_dir))
    return measurements


def measurement_counts() -> Dict[str, object]:
    """The sample counts the latency rows were recorded with; stored in the budget file."""
    return {
        "cold_runs": COLD_RUNS,
        "edit_probe_runs": EDIT_PROBE_RUNS,
        "edit_samples_per_run": EDIT_SAMPLES,
        "edit_warmup": EDIT_WARMUP,
        # The statistic is part of the claim: a budget recorded as a minimum is not the same claim as
        # one recorded as a mean, so the file records which it is.
        "statistic": "min",
    }


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
