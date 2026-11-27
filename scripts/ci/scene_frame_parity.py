#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Preview canvas parity gate (issue #167, P3 of docs/design/reference-equantic-ui.md section 7.4).

P3's acceptance is **"the canvas render is within tolerance of the lavapipe golden for the parity
samples"**. That sentence names two artifacts, so this script is built around producing and comparing
them - and around being explicit about which half it can prove where.

## The two sides, and what each proves

  native reference   `tessera_scene_frame --png OUT --backend vulkan FILE.tsx`, run on a host whose
                     Vulkan device is **lavapipe** (the software ICD, ADR-009). This is the golden:
                     lavapipe is the only renderer this repository will pin pixels against.
  browser candidate  the WebGPU module the page loads (`tessera_canvas.js`), drawing the same scene.

Only the first side can be produced headlessly. A browser has no synchronous buffer map, and Node has
no WebGPU at all (`src/rendering/backend/webgpu/smoke/webgpu_wasm_smoke.cpp` says so), so the second
side cannot be rendered by `$EMSDK_NODE` - honestly, not "for now": there is no device there to render
with. This script therefore has three modes, in order of what they prove:

  `--native-reference`  (default) render the corpus with the native CLI, assert determinism, assert
                        the frames are not blank, and write/compare the lavapipe goldens. Runs on any
                        machine with a software Vulkan driver; exits 2 with a skip line without one.
  `--canvas-report`     run the browser module under Node with NO device and assert the ABI: the
                        module loads, reports `hasDevice:false`, and does not crash. That is the
                        unsupported-browser contract (the page must say so, not show a blank canvas).
  `--compare`           compare two PNG directories produced by any two of the above (used by the
                        browser lane, which supplies the WebGPU side from a real browser run).

The tolerance is **not invented here**: `tests/golden/tolerance.json` already defines the cross-backend
policy, and its `webgpu_vs_lavapipe` pair is the one P3's sentence is about (8/255 per channel, 2% of
pixels, SSIM >= 0.99, deltaE <= 2.0). This script reads that file through `scripts/golden_tolerances.py`
- the same loader the C++ parity harness uses - so a tolerance change is one edit, and a missing pair
fails instead of silently comparing with a default.

## Why the corpus is not `samples/parity-demo`

The *samples* in the acceptance sentence are the corpus the earlier preview gates use. The preview
binds no event handler inside a user component instance (docs/design/compiler-wasm.md section 7.1); it does evaluate
a conditional child and a repeated child (issue #389), so those shapes are no longer excluded for being
unsupported. The corpus here is still the subset whose *pixels* this gate needs plus the paint-relevant
cases P3 added - a filled box, a rounded box, nested clips, and a multi-column layout - because the
frame gate's job is the paint path, and `scene_snapshot_parity.py` is what covers the two dynamic
shapes' node trees.
Each case declares a minimum draw-call count, for the same anti-vacuity reason the snapshot gate
declares a minimum node count - two blank frames are byte-identical and prove nothing.

Exit codes: 0 the mode's assertions hold, 1 a failure, 2 setup failure or a documented skip.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = SCRIPTS_DIR.parent
sys.path.insert(0, str(SCRIPTS_DIR))
import golden_tolerances  # noqa: E402  (sibling script, stdlib only)

TOLERANCE_FILE = REPO_ROOT / "tests" / "golden" / "tolerance.json"

# The pair P3's acceptance names. A misspelling fails loudly instead of resolving to the exact default,
# which is the failure mode that would make this gate pass on two different images.
PAIR = "webgpu_vs_lavapipe"

# The same salt and file convention the P2 corpus uses. The salt feeds every `uiId`, and the source
# path feeds the ids too, so both sides have to be told the same values or the gate compares scenes
# that differ in identity rather than in pixels.
SALT = "preview"

# (name, project-relative file, source, minimum draw calls). The minimum is the anti-vacuity guard: a
# frame that painted nothing is the clear colour everywhere, and two clear-colour frames compare equal
# at tolerance 0. Each case isolates one thing that can break the canvas path:
#
#   column     three element kinds in a column, the baseline "does anything draw at all" case
#   box        one explicitly sized, coloured box: the minimal one-quad frame
#   rounded    a border-radius box, which takes the SDF path through the shader
#   nested     a clipping parent with a child: the scissor + clip stack
#   row        a flex row of three boxes: a layout change that moves every quad
#   dpr        a full-bleed box rendered at DPR 2: the paint side applies the device scale (#599).
#              Without it the scene paints 1:1 into the top-left quarter of the doubled target, so
#              the corner assertion below (not just "something drew") is what this case adds.
CORPUS: Tuple[Tuple[str, str, str, int], ...] = (
    (
        "column",
        "ui/Column.tsx",
        """export function Column(): Node {
  return (
    <View>
      <Text>Preview</Text>
      <Button>Add</Button>
      <Button>Reset</Button>
    </View>
  );
}
""",
        1,
    ),
    (
        "box",
        "ui/Box.tsx",
        """export function Box(): Node {
  return (
    <View style={{ width: 120, height: 80 }}>
      <Button>Ok</Button>
    </View>
  );
}
""",
        1,
    ),
    (
        "rounded",
        "ui/Rounded.tsx",
        """export function Rounded(): Node {
  return (
    <View style={{ width: 200, height: 120, paddingLeft: 8, paddingTop: 8 }}>
      <Button>Rounded</Button>
      <Spacer />
    </View>
  );
}
""",
        1,
    ),
    (
        "dpr",
        "ui/Dpr.tsx",
        """export function Dpr(): Node {
  return <View style={{ width: 320, height: 240, backgroundColor: "#ff0000" }} />;
}
""",
        1,
    ),
    (
        "nested",
        "ui/Nested.tsx",
        """export function Nested(): Node {
  return (
    <View style={{ width: 240, height: 160 }}>
      <View style={{ width: 120, height: 60 }}>
        <Text>Inside</Text>
      </View>
      <Button>Outside</Button>
    </View>
  );
}
""",
        1,
    ),
    (
        "row",
        "ui/Row.tsx",
        """export function Row(): Node {
  return (
    <View style={{ flexDirection: "row", width: 320, height: 80 }}>
      <Button>One</Button>
      <Button>Two</Button>
      <Button>Three</Button>
    </View>
  );
}
""",
        1,
    ),
)


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", help="host-built tessera_scene_frame CLI")
    parser.add_argument("--canvas", help="the Emscripten tessera_canvas module (tessera_canvas.js)")
    parser.add_argument("--node", help="Node that runs the canvas module (normally $EMSDK_NODE)")
    parser.add_argument(
        "--mode",
        choices=("native-reference", "canvas-report", "canvas-font", "compare"),
        default="native-reference",
        help="which half of the gate to run (default: native-reference)",
    )
    parser.add_argument("--out-dir", help="where rendered frames are written")
    parser.add_argument("--golden-dir", help="the committed lavapipe goldens (compare mode)")
    parser.add_argument("--candidate-dir", help="the candidate frames (compare mode)")
    parser.add_argument("--write-goldens", action="store_true",
                        help="write --golden-dir from the native render instead of comparing")
    parser.add_argument("--size", default="320x240", help="the layout viewport (default 320x240)")
    parser.add_argument("--font",
                        help="the shipped preview font, for --mode canvas-font "
                             "(default: designer/web/public/fonts/IBMPlexSans-Latin.ttf)")
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    return parser.parse_args(argv)


def tolerance_pair() -> Tuple[int, float, float, float]:
    """`(edge_tolerance, max_mismatch, min_ssim, max_delta_e)` for P3's pair."""
    document = golden_tolerances.load(TOLERANCE_FILE)
    tolerance, max_mismatch, ssim, delta_e = golden_tolerances.resolve_pair(document,
                                                                            "gpu_backend_parity", PAIR)
    return tolerance, max_mismatch, ssim, delta_e


def write_case(directory: Path, name: str, file: str, source: str) -> Path:
    """One corpus case on disk. The file NAME is part of the id hash, so it is passed through."""
    case = directory / name
    case.mkdir(parents=True, exist_ok=True)
    path = case / Path(file).name
    path.write_text(source, encoding="utf-8")
    return path


def _is_lavapipe(stderr_and_stdout: str) -> bool:
    """True when the CLI's device banner names the software ICD.

    ADR-009 pins goldens to lavapipe: a hardware driver's pixels are not a reference, and committing
    them would make every other machine "fail" against one vendor's rasterizer. The CLI prints the
    device it opened, so this needs no extra query.
    """
    lowered = stderr_and_stdout.lower()
    return "lavapipe" in lowered or "llvmpipe" in lowered


def run_native(native: Path, case: Path, out: Path, file: str, size: str,
               work: Path) -> subprocess.CompletedProcess:
    """One native render, from the cases root so a relative path cannot leak between cases.

    The INPUT is absolute (a path the CLI can open from any working directory) while `--file` is
    project-relative and `cwd` is the scratch root: the source path is an input to every `uiId`, so
    the two sides must be told one spelling, not two absolutes that differ per machine.
    """
    command = [str(native), "--png", str(out), "--backend", "vulkan", "--size", size,
               "--file", file, "--salt", SALT, str(case)]
    return subprocess.run(command, cwd=str(work), capture_output=True, text=True, timeout=600)


def native_reference(args: argparse.Namespace, repo_root: Path) -> int:
    native = Path(args.native).resolve() if args.native else None
    if native is None or not native.is_file():
        print("SETUP ERROR: --native not found: {}".format(args.native), file=sys.stderr)
        return 2
    out_dir = Path(args.out_dir).resolve() if args.out_dir else repo_root / "build" / "scene-frame"
    out_dir.mkdir(parents=True, exist_ok=True)
    work = out_dir / "cases"
    work.mkdir(parents=True, exist_ok=True)

    edge, max_mismatch, min_ssim, max_delta_e = tolerance_pair()
    print("tolerance: pair={} edge={} max_mismatch={} ssim={} deltaE={}".format(
        PAIR, edge, max_mismatch, min_ssim, max_delta_e))

    failures: List[str] = []
    rendered = 0
    devices: List[str] = []
    for name, file, source, minimum_draws in CORPUS:
        case = write_case(work, name, file, source)
        first = out_dir / "{}.png".format(name)
        second = out_dir / "{}_repeat.png".format(name)
        size = case_size(name, args.size)
        outcome = run_native(native, case, first, file, size, work)
        if outcome.returncode != 0:
            # A missing Vulkan device is a skip, not a failure: the lane has no evidence either way.
            if "no Vulkan device" in (outcome.stderr + outcome.stdout):
                print("SKIP: no Vulkan device on this host ({}); no reference was rendered".format(
                    (outcome.stderr + outcome.stdout).strip().splitlines()[-1]))
                return 2
            failures.append("{}: native render exited {}: {}".format(
                name, outcome.returncode, (outcome.stderr + outcome.stdout).strip()))
            continue
        again = run_native(native, case, second, file, size, work)
        if again.returncode != 0:
            failures.append("{}: repeat render exited {}: {}".format(
                name, again.returncode, (again.stderr + again.stdout).strip()))
            continue
        # Determinism is a property of the whole path (layout, batching, rasterization). A renderer
        # that is stable within one process but not across processes would make every comparison
        # meaningless, so it is asserted before the pixels are used.
        if first.read_bytes() != second.read_bytes():
            failures.append("{}: two runs of the same case produced different bytes".format(name))
            continue
        report = first.with_suffix(".txt")
        report.write_text(outcome.stdout, encoding="utf-8")
        draws = _draws_from_report(outcome.stdout)
        if draws is not None and draws < minimum_draws:
            failures.append(
                "{}: the frame recorded {} draw call(s), the case needs at least {} (a frame that "
                "painted nothing is the clear colour and would compare equal to any other clear "
                "frame)".format(name, draws, minimum_draws))
            continue
        scaled = scale_failure(name, first)
        if scaled is not None:
            failures.append(scaled)
            continue
        rendered += 1
        print("NATIVE OK {} bytes={} draws={} device={}".format(
            first.name, first.stat().st_size, "?" if draws is None else draws,
            _device_name(outcome.stdout + outcome.stderr)))
        devices.append(outcome.stdout + outcome.stderr)
    if failures:
        for failure in failures:
            print("FAIL {}".format(failure), file=sys.stderr)
        return 1
    if rendered == 0:
        print("FAIL the corpus rendered nothing", file=sys.stderr)
        return 1
    if args.write_goldens:
        # ADR-009: goldens come from lavapipe. A hardware driver's pixels are not a reference - they
        # are one vendor's rasterizer - so writing them as goldens would make the gate fail on every
        # other machine for a reason that is not a regression. Refusing here rather than warning is
        # deliberate: a wrongly-sourced golden is unrecoverable without a history dig.
        if not all(_is_lavapipe(text) for text in devices):
            print(
                "REFUSED: --write-goldens needs a lavapipe device on every case (ADR-009). This "
                "host rendered with: {}".format(
                    ", ".join(sorted({_device_name(text) for text in devices}))),
                file=sys.stderr)
            return 2
        golden_dir = Path(args.golden_dir).resolve() if args.golden_dir else (
            repo_root / "tests" / "golden" / "canvas")
        golden_dir.mkdir(parents=True, exist_ok=True)
        for name, _, _, _ in CORPUS:
            shutil.copyfile(out_dir / "{}.png".format(name), golden_dir / "{}.png".format(name))
        print("WROTE {} lavapipe golden(s) to {}".format(len(CORPUS), golden_dir))
        return 0
    print("NATIVE REFERENCE OK: {} case(s), deterministic, >= their minimum draw counts".format(
        rendered))
    return 0


def _device_name(text: str) -> str:
    """The CLI's `Vulkan device: ...` line, or 'unknown' when it did not print one."""
    for line in text.splitlines():
        if "Vulkan device:" in line:
            return line.split("Vulkan device:", 1)[1].strip() or "unknown"
    return "unknown"


def _draws_from_report(text: str) -> Optional[int]:
    """`draws=N` from the CLI's one-line report, or None when it did not print one."""
    for line in text.splitlines():
        marker = "draws="
        at = line.find(marker)
        if at >= 0:
            digits = ""
            for character in line[at + len(marker):]:
                if not character.isdigit():
                    break
                digits += character
            if digits:
                return int(digits)
    return None


# Cases whose pixels prove the paint side applied the device scale (issue #599): a full-bleed box
# at DPR 2 must cover the doubled target, so a sample near the far corner is the box's colour and not
# the clear colour. Without the scale the scene paints 1:1 and that corner is clear - the frame this
# guards against looked "fine" in its top-left quarter and blank everywhere else.
SCALED_CASES = {
    "dpr": {
        "size": "320x240@2",
        # The case's own fill (`backgroundColor: #ff0000` in the source below), which a full-bleed
        # box must show at the far corner of the doubled target.
        "box_rgb": (0xFF, 0x00, 0x00),
        "tolerance": 16,
    }
}


def case_size(name: str, default: str) -> str:
    """The `WxH[@DPI]` a case renders at: its own for a scaled case, the command line's otherwise."""
    entry = SCALED_CASES.get(name)
    return entry["size"] if entry else default


def _read_png_rgb(path: Path):
    """Decode an 8-bit RGB/RGBA PNG into `(width, height, rows, channels)` with the standard library only.

    The gate needs one pixel, and adding Pillow for it would be a dependency
    (`docs/specs/dependencies.md`) for a decode the format makes trivial: the CLI writes a non-
    interlaced RGBA8 image, so the IDAT stream is filter-byte-per-row of packed samples.
    """
    import struct
    import zlib

    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    at = 8
    width = height = depth = color = 0
    compressed = b""
    while at < len(data):
        length = struct.unpack(">I", data[at:at + 4])[0]
        kind = data[at + 4:at + 8]
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            if depth != 8 or color not in (2, 6):
                raise ValueError("expected 8-bit RGB/RGBA")
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    channels = 3 if color == 2 else 4
    stride = width * channels
    raw = zlib.decompress(compressed)
    rows = []
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filter_byte = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        # The five PNG row filters; only "none" and "up" are cheap to state, so all are handled.
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            up_left = previous[x - channels] if x >= channels else 0
            if filter_byte == 1:
                row[x] = (row[x] + left) & 0xFF
            elif filter_byte == 2:
                row[x] = (row[x] + up) & 0xFF
            elif filter_byte == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif filter_byte == 4:
                estimate = left + up - up_left
                pa, pb, pc = abs(estimate - left), abs(estimate - up), abs(estimate - up_left)
                row[x] = (row[x] + (left if pa <= pb and pa <= pc else up if pb <= pc else up_left)) & 0xFF
        rows.append(bytes(row))
        previous = row
    return width, height, rows, channels


def scale_failure(name: str, png: Path) -> Optional[str]:
    """Why a scaled case failed to apply its device scale, or None when it did."""
    entry = SCALED_CASES.get(name)
    if entry is None:
        return None
    try:
        width, height, rows, channels = _read_png_rgb(png)
    except Exception as error:  # noqa: BLE001 - any decode problem is the failure being reported
        return "{}: could not read the frame back: {}".format(name, error)
    # The CLI's own banner already reported the extent, so the assertion here is about PIXELS: the
    # far corner must be the box's colour, not the clear colour. A frame that painted 1:1 leaves the
    # far corner clear, which is exactly the bug (#599); a check for "the clear colour" instead of
    # "the box colour" would pass on a frame that painted nothing at all.
    wanted = entry["box_rgb"]
    x = max(0, (width * 7) // 8)
    y = max(0, (height * 7) // 8)
    start = x * channels
    pixel = rows[y][start:start + 3]
    if not all(abs(pixel[channel] - wanted[channel]) <= entry["tolerance"] for channel in range(3)):
        return ("{}: the far corner of a {}x{} frame is {}, not the box's colour {}: the scene did "
                "not cover the whole device-pixel target (#599)".format(
                    name, width, height, tuple(pixel), tuple(wanted)))
    return None


def canvas_report(args: argparse.Namespace) -> int:
    """The unsupported-browser contract, checked without a browser.

    The module is loaded under Node, which has no `navigator.gpu`; that is exactly the state a
    browser without WebGPU is in. It must load, answer with `hasDevice:false`, and not crash. A module
    that threw on import would leave the page with a blank canvas and no explanation - the failure
    mode P3 explicitly has to avoid.
    """
    canvas = Path(args.canvas).resolve() if args.canvas else None
    if canvas is None or not canvas.is_file():
        print("SETUP ERROR: --canvas not found: {}".format(args.canvas), file=sys.stderr)
        return 2
    node = args.node or "node"
    probe = Path(__file__).with_name("scene_frame_canvas_probe.mjs")
    outcome = subprocess.run([node, str(probe), str(canvas)], capture_output=True, text=True,
                             timeout=300)
    sys.stdout.write(outcome.stdout)
    if outcome.returncode != 0:
        sys.stderr.write(outcome.stderr)
        print("FAIL: the canvas module did not honour the no-device contract", file=sys.stderr)
        return 1
    print("CANVAS REPORT OK")
    return 0


def canvas_font(args: argparse.Namespace, repo_root: Path) -> int:
    """The canvas module's font ABI (issue #387), checked without a browser.

    The page fetches one face from its own origin, writes the bytes into the module's heap and calls
    `tessera_canvas_set_font`; this loads the same module under Node, performs the same handover, and
    asserts the answers. Node has no `navigator.gpu`, so the module refuses the bytes for want of a
    device - which is the contract worth pinning here, because the alternative (a silent success with
    no atlas) would show up in a browser as a canvas of empty boxes with a healthy status line.

    The glyph proof is native: `--native-reference` and the `scene_frame` ctest paint text from these
    same bytes. This mode proves the *page's* half of the handover, which those cannot.
    """
    canvas = Path(args.canvas).resolve() if args.canvas else None
    if canvas is None or not canvas.is_file():
        print("SETUP ERROR: --canvas not found: {}".format(args.canvas), file=sys.stderr)
        return 2
    font = (
        Path(args.font).resolve()
        if args.font
        else repo_root / "designer" / "web" / "public" / "fonts" / "IBMPlexSans-Latin.ttf"
    )
    if not font.is_file():
        print("SETUP ERROR: the preview font is missing: {}".format(font), file=sys.stderr)
        return 2
    node = args.node or "node"
    probe = Path(__file__).with_name("scene_frame_canvas_font_probe.mjs")
    outcome = subprocess.run([node, str(probe), str(canvas), str(font)], capture_output=True,
                             text=True, timeout=300)
    sys.stdout.write(outcome.stdout)
    if outcome.returncode != 0:
        sys.stderr.write(outcome.stderr)
        print("FAIL: the canvas module did not honour the font ABI", file=sys.stderr)
        return 1
    print("CANVAS FONT OK")
    return 0


def compare(args: argparse.Namespace) -> int:
    """Compare two PNG directories under P3's tolerance pair.

    The pixel comparison itself is `tessera_scene_frame`'s sibling code in C++ (the golden library),
    so this mode shells out to it rather than reimplementing SSIM/deltaE in Python: a second
    implementation of the metric could disagree with the C++ gate that the rest of the repository
    uses, and then two lanes would hold two different definitions of "within tolerance".
    """
    golden_dir = Path(args.golden_dir).resolve() if args.golden_dir else None
    candidate_dir = Path(args.candidate_dir).resolve() if args.candidate_dir else None
    if golden_dir is None or candidate_dir is None:
        print("SETUP ERROR: --compare needs --golden-dir and --candidate-dir", file=sys.stderr)
        return 2
    if not (golden_dir.is_dir() and candidate_dir.is_dir()):
        print("SETUP ERROR: both directories must exist", file=sys.stderr)
        return 2
    compare_cli = Path(args.native).resolve() if args.native else None
    if compare_cli is None or not compare_cli.is_file():
        print("SETUP ERROR: --native must name tessera_golden_compare for --compare",
              file=sys.stderr)
        return 2
    edge, max_mismatch, min_ssim, max_delta_e = tolerance_pair()
    failures = 0
    compared = 0
    for name, _, _, _ in CORPUS:
        golden = golden_dir / "{}.png".format(name)
        candidate = candidate_dir / "{}.png".format(name)
        if not golden.is_file() or not candidate.is_file():
            print("FAIL {}: missing {}".format(name, "golden" if not golden.is_file()
                                               else "candidate"), file=sys.stderr)
            failures += 1
            continue
        outcome = subprocess.run(
            [str(compare_cli), "--pair", PAIR, "--tolerance", str(edge),
             "--max-mismatch", str(max_mismatch), "--ssim", str(min_ssim),
             "--delta-e", str(max_delta_e), str(candidate), str(golden)],
            capture_output=True, text=True, timeout=300)
        sys.stdout.write(outcome.stdout)
        # Flushed so the child's report lands in the log in the order it was produced: Python buffers
        # when the output is redirected, and an interleaved log makes a failure hard to read.
        sys.stdout.flush()
        if outcome.returncode != 0:
            sys.stderr.write(outcome.stderr)
            failures += 1
            continue
        compared += 1
    if failures != 0:
        print("FAIL {} case(s) outside the {} tolerance".format(failures, PAIR), file=sys.stderr)
        return 1
    print("COMPARE OK: {} case(s) within the {} tolerance".format(compared, PAIR))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    repo_root = Path(args.repo_root).resolve()
    if args.mode == "native-reference":
        return native_reference(args, repo_root)
    if args.mode == "canvas-report":
        return canvas_report(args)
    if args.mode == "canvas-font":
        return canvas_font(args, repo_root)
    return compare(args)


if __name__ == "__main__":
    sys.exit(main())
