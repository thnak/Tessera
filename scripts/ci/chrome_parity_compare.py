#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare the engine's capture of the parity form with Chrome's.

Inputs, in one directory (`--dir`): `engine.png` (from `gallery --page form --capture`) and
`chrome.png` plus `chrome-boxes.json` (from `scripts/ci/chrome_parity.mjs`).

Outputs, in the same directory:
  side_by_side.png   engine | chrome
  diff.png           per-pixel difference, white = identical
  overlay.png        the engine capture with the outline of every Chrome element box on top, so a
                     layout difference reads as an outline that does not hug its element
  report.json        the numbers below

Numbers: the share of pixels that differ by more than a small threshold, the mean absolute channel
difference, and a *structure* score computed on a blurred, downscaled copy of both images, which
ignores glyph-edge anti-aliasing and so measures where things are, not how their edges are filtered.
Text rasterization always differs between a browser and the engine; read the structure score first.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def load(path: Path) -> Image.Image:
    return Image.open(path).convert("RGB")


def structure(a: Image.Image, b: Image.Image, scale: int = 4, blur: float = 2.0) -> float:
    """Mean absolute difference (0..1) of blurred, downscaled copies."""
    small = (max(1, a.width // scale), max(1, a.height // scale))
    fa = np.asarray(a.resize(small, Image.BILINEAR).filter(ImageFilter.GaussianBlur(blur)), dtype=np.float32)
    fb = np.asarray(b.resize(small, Image.BILINEAR).filter(ImageFilter.GaussianBlur(blur)), dtype=np.float32)
    return float(np.abs(fa - fb).mean() / 255.0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True)
    parser.add_argument("--threshold", type=int, default=24, help="per-channel difference that counts as different")
    args = parser.parse_args()
    d = Path(args.dir)
    engine, chrome = load(d / "engine.png"), load(d / "chrome.png")
    if engine.size != chrome.size:
        print(f"size mismatch: engine {engine.size}, chrome {chrome.size}", file=sys.stderr)
        return 2

    ea, ca = np.asarray(engine, dtype=np.int16), np.asarray(chrome, dtype=np.int16)
    delta = np.abs(ea - ca)
    differing = (delta.max(axis=2) > args.threshold)
    report = {
        "size": list(engine.size),
        "threshold": args.threshold,
        "differing_pixels_pct": round(float(differing.mean() * 100), 2),
        "mean_abs_channel_diff": round(float(delta.mean()), 2),
        "structure_diff_pct": round(structure(engine, chrome) * 100, 2),
    }

    sbs = Image.new("RGB", (engine.width * 2 + 8, engine.height), (255, 0, 255))
    sbs.paste(engine, (0, 0))
    sbs.paste(chrome, (engine.width + 8, 0))
    sbs.save(d / "side_by_side.png")

    diff = Image.fromarray((255 - np.clip(delta.max(axis=2) * 2, 0, 255)).astype(np.uint8)).convert("RGB")
    diff.save(d / "diff.png")

    overlay = engine.copy()
    draw = ImageDraw.Draw(overlay)
    boxes = json.loads((d / "chrome-boxes.json").read_text(encoding="utf-8"))
    for box in boxes:
        if box["w"] < 2 or box["h"] < 2:
            continue
        draw.rectangle([box["x"], box["y"], box["x"] + box["w"] - 1, box["y"] + box["h"] - 1], outline=(255, 0, 0))
    overlay.save(d / "overlay.png")

    (d / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
