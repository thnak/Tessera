// SPDX-License-Identifier: MIT
//
// The fold-hinge and safe-area overlay's geometry (issue #433), tested against the module the page
// ships.
//
//   node scripts/ci/preview_fold_overlay.mjs
//
// Needs only the SSR build (`npm run build:chrome --prefix designer/web`): the arithmetic is pure, and
// the DOM half (the toggle, the boxes, the caption) is a browser gate - `preview_browser_canvas_size`
// is its neighbour and the pattern to follow. What is worth pinning here is the two rules the overlay
// exists to keep:
//
//   1. The boxes are the rectangles the ENGINE reported, scaled - never recomputed. A `book` fold's
//      band is whatever `hal::fold_info_for` centred, and the overlay's job is to put that rectangle
//      where the layout put it.
//   2. Nothing is drawn for a device with no fold and no insets, so arming the overlay on an ordinary
//      desktop preview leaves the picture exactly as it was.

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const MODULE = resolve(HERE, "..", "..", "designer", "web", "dist-ssr", "canvas-fold.js");

let failures = 0;
function test(name, body) {
  try {
    body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

let fold;
try {
  fold = await import(pathToFileURL(MODULE).href);
} catch {
  process.stderr.write(
    `SETUP ERROR: ${MODULE} is missing. Build the page first:\n` +
      "  npm ci --prefix designer/web && npm run build:chrome --prefix designer/web\n",
  );
  process.exit(2);
}

const { overlayBoxes, overlayCaption, resolvedInsets } = fold;

/** The report `_tessera_canvas_overlay` writes for a `book` fold, as the engine writes it. */
function bookReport() {
  return {
    width: 800,
    height: 600,
    foldState: "half-opened",
    foldOrientation: "vertical",
    foldOcclusion: "full",
    separating: true,
    hingeAngle: -1,
    // (800 - 24) / 2 = 388, full height: what `fold_info_for` centres.
    hinge: { x: 388, y: 0, width: 24, height: 600 },
    insets: { left: 0, top: 0, right: 0, bottom: 0 },
    ime: { left: 0, top: 0, right: 0, bottom: 0 },
    gestures: { left: 0, top: 0, right: 0, bottom: 0 },
    cutouts: [],
  };
}

/** The same report with no fold at all: an unfolded device. */
function desktopReport() {
  return {
    width: 1280,
    height: 800,
    foldState: "none",
    foldOrientation: "none",
    foldOcclusion: "none",
    separating: false,
    hingeAngle: -1,
    hinge: { x: 0, y: 0, width: 0, height: 0 },
    insets: { left: 0, top: 0, right: 0, bottom: 0 },
    ime: { left: 0, top: 0, right: 0, bottom: 0 },
    gestures: { left: 0, top: 0, right: 0, bottom: 0 },
    cutouts: [],
  };
}

test("the hinge band is the engine's rectangle, at scale 1", () => {
  const boxes = overlayBoxes(bookReport(), 1);
  const hinge = boxes.filter((box) => box.kind === "hinge");
  assert.equal(hinge.length, 1, "a book fold draws exactly one band");
  assert.deepEqual(hinge[0], {
    kind: "hinge",
    left: 388,
    top: 0,
    width: 24,
    height: 600,
  });
});

test("a pane that shrank the canvas scales every box by the same factor", () => {
  // A narrow pane shrinks the canvas' CSS box (`max-width: 100%`), so one logical pixel is less than
  // one CSS pixel. Every box has to move by the same factor or the band stops matching the layout.
  const boxes = overlayBoxes(bookReport(), 0.5);
  const hinge = boxes.find((box) => box.kind === "hinge");
  assert.deepEqual(hinge, { kind: "hinge", left: 194, top: 0, width: 12, height: 300 });
});

test("a bad scale cannot collapse or invert the overlay", () => {
  for (const bad of [0, -1, NaN, Infinity]) {
    const boxes = overlayBoxes(bookReport(), bad);
    const hinge = boxes.find((box) => box.kind === "hinge");
    assert.deepEqual(
      hinge,
      { kind: "hinge", left: 388, top: 0, width: 24, height: 600 },
      `scale ${bad} is treated as 1`,
    );
  }
});

test("an unfolded desktop draws nothing at all", () => {
  // The toggle must be free on an ordinary preview: arming it and seeing no shading is the correct
  // answer, not a broken overlay.
  assert.deepEqual(overlayBoxes(desktopReport(), 1), []);
});

test("a flat foldable has a state but no band", () => {
  const report = desktopReport();
  report.foldState = "flat";
  assert.deepEqual(overlayBoxes(report, 1), [], "a flat fold has nothing to shade");
  assert.equal(overlayCaption(report), "flat", "but the caption names the posture");
});

test("each inset side is one band, and the middle stays clear", () => {
  const report = desktopReport();
  report.insets = { left: 0, top: 47, right: 0, bottom: 34 };
  const boxes = overlayBoxes(report, 1);
  const top = boxes.find((box) => box.kind === "inset" && box.top === 0);
  const bottom = boxes.find((box) => box.kind === "inset" && box.top !== 0);
  assert.deepEqual(top, { kind: "inset", left: 0, top: 0, width: 1280, height: 47 });
  assert.deepEqual(bottom, { kind: "inset", left: 0, top: 800 - 34, width: 1280, height: 34 });
  assert.equal(boxes.length, 2, "only the sides with an inset are shaded");
});

test("the left and right bands span the full height", () => {
  const report = desktopReport();
  report.insets = { left: 12, top: 0, right: 8, bottom: 0 };
  const boxes = overlayBoxes(report, 1);
  const left = boxes.find((box) => box.kind === "inset" && box.left === 0);
  const right = boxes.find((box) => box.kind === "inset" && box.left !== 0);
  assert.deepEqual(left, { kind: "inset", left: 0, top: 0, width: 12, height: 800 });
  assert.deepEqual(right, { kind: "inset", left: 1280 - 8, top: 0, width: 8, height: 800 });
});

test("the three inset sources resolve per side by their maximum", () => {
  // The same rule `hal::resolve_safe_area` applies, for the same reason: they overlap (an IME inset
  // already includes the bars under it), so adding them would shade space the content kept.
  const insets = resolvedInsets({
    insets: { left: 0, top: 47, right: 0, bottom: 34 },
    ime: { left: 0, top: 0, right: 0, bottom: 291 },
    gestures: { left: 0, top: 0, right: 0, bottom: 20 },
  });
  assert.deepEqual(insets, { left: 0, top: 47, right: 0, bottom: 291 });
});

test("a cutout is one box per reported rectangle", () => {
  const report = desktopReport();
  report.cutouts = [{ x: 130, y: 0, width: 130, height: 30 }];
  const cutouts = overlayBoxes(report, 1).filter((box) => box.kind === "cutout");
  assert.deepEqual(cutouts, [
    { kind: "cutout", left: 130, top: 0, width: 130, height: 30 },
  ]);
});

test("a zero-area box is not drawn", () => {
  // A device that reports a cutout with no area (or a hint band that is zero) must not leave a
  // hairline on the picture.
  const report = desktopReport();
  report.cutouts = [{ x: 10, y: 10, width: 0, height: 20 }];
  assert.deepEqual(overlayBoxes(report, 1), []);
});

test("the caption names the posture and the band's thickness", () => {
  assert.match(overlayCaption(bookReport()), /vertical hinge 24px/);
});

test("a simulated posture's unknown angle is not printed as a number", () => {
  // -1 is the HAL's "nothing measured one". A caption saying `-1°` would be a number a developer
  // could believe.
  const caption = overlayCaption(bookReport());
  assert.doesNotMatch(caption, /-1/);
  assert.doesNotMatch(caption, /°/);
});

test("a measured angle is printed", () => {
  const report = bookReport();
  report.hingeAngle = 90;
  assert.match(overlayCaption(report), /90°/);
});

test("an unfolded desktop's caption is a named absence", () => {
  assert.equal(overlayCaption(desktopReport()), "no fold");
});

process.stdout.write(
  failures === 0
    ? `\npreview_fold_overlay: all checks passed\n`
    : `\npreview_fold_overlay: ${failures} check(s) failed\n`,
);
process.exit(failures === 0 ? 0 : 1);
