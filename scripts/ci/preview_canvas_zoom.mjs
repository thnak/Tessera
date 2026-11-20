// SPDX-License-Identifier: MIT
//
// The canvas pane's zoom arithmetic (owner report: the Studio hides part of the canvas; it needs zoom and
// move). The pane scales the stage with a CSS transform and scrolls the frame, so the numbers that decide
// what the person sees are pure functions, and they are tested here against the module the page ships.
//
//   node scripts/ci/preview_canvas_zoom.mjs
//
// Needs the SSR build (`npm run build --prefix designer/web`). No browser: how the pane lays the stage out
// and how the pointer maps through the transform are covered by the page checks and by the real-browser
// check the PR records.

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const MODULE = resolve(HERE, "..", "..", "designer", "web", "dist-ssr", "canvas-zoom.js");

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

let zoom;
try {
  zoom = await import(pathToFileURL(MODULE).href);
} catch {
  process.stderr.write(
    `SETUP ERROR: ${MODULE} is missing. Build the page first:\n` +
      "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
  );
  process.exit(2);
}
const { MIN_ZOOM, MAX_ZOOM, ZOOM_STEPS, clampZoom, fitZoom, stepZoom, zoomLabel, anchoredScroll, cssZoomOf } = zoom;

test("fit shows the whole stage: the tighter axis decides", () => {
  assert.equal(fitZoom({ width: 500, height: 500 }, { width: 1000, height: 400 }), 0.5);
  assert.equal(fitZoom({ width: 500, height: 200 }, { width: 400, height: 1000 }), 0.2);
});

test("fit never enlarges past 100%", () => {
  assert.equal(fitZoom({ width: 2000, height: 2000 }, { width: 320, height: 240 }), 1);
});

test("fit with no size yet (a hidden pane) is 100%, not a divide by zero", () => {
  assert.equal(fitZoom({ width: 0, height: 0 }, { width: 320, height: 240 }), 1);
  assert.equal(fitZoom({ width: 500, height: 500 }, { width: 0, height: 0 }), 1);
});

test("fit is held inside the zoom range", () => {
  assert.equal(fitZoom({ width: 10, height: 10 }, { width: 5000, height: 5000 }), MIN_ZOOM);
});

test("clamp keeps a zoom inside the range and turns nonsense into 100%", () => {
  assert.equal(clampZoom(100), MAX_ZOOM);
  assert.equal(clampZoom(0.001), MIN_ZOOM);
  for (const bad of [NaN, Infinity, -1, 0]) {
    assert.equal(clampZoom(bad), 1, String(bad));
  }
});

test("the steps are increasing and 100% is one of them", () => {
  for (let index = 1; index < ZOOM_STEPS.length; index += 1) {
    assert.ok(ZOOM_STEPS[index] > ZOOM_STEPS[index - 1], `step ${index}`);
  }
  assert.ok(ZOOM_STEPS.includes(1));
});

test("stepping moves to the next level from a level and from between two", () => {
  assert.equal(stepZoom(1, 1), 1.25);
  assert.equal(stepZoom(1, -1), 0.75);
  // A fitted zoom is rarely a level; stepping goes to the neighbour in that direction.
  assert.equal(stepZoom(0.57, 1), 0.67);
  assert.equal(stepZoom(0.57, -1), 0.5);
});

test("stepping stops at the ends of the list", () => {
  assert.equal(stepZoom(MAX_ZOOM, 1), MAX_ZOOM);
  assert.equal(stepZoom(MIN_ZOOM, -1), MIN_ZOOM);
});

test("the label is a whole percent", () => {
  assert.equal(zoomLabel(1), "100%");
  assert.equal(zoomLabel(0.67), "67%");
  assert.equal(zoomLabel(1.31629), "132%");
});

test("a wheel zoom keeps the point under the cursor still", () => {
  // The point at content offset p sits at screen offset p * zoom - scroll. Anchoring keeps that equal for
  // the cursor before and after.
  const scroll = 120;
  const cursor = 200;
  const before = 1;
  const after = 2;
  const next = anchoredScroll(scroll, cursor, before, after);
  const contentUnderCursor = (scroll + cursor) / before;
  assert.equal(contentUnderCursor * after - next, cursor);
});

test("anchoring never scrolls to a negative offset", () => {
  assert.equal(anchoredScroll(0, 10, 1, 0.1), 0);
  assert.equal(anchoredScroll(50, 10, 0, 2), 50);
});

test("the css zoom is the transformed size over the layout size", () => {
  assert.equal(cssZoomOf(160, 320), 0.5);
  assert.equal(cssZoomOf(320, 320), 1);
  // No layout width (hidden) or no rect: unscaled, not NaN.
  assert.equal(cssZoomOf(0, 0), 1);
  assert.equal(cssZoomOf(100, 0), 1);
});

process.stdout.write(failures === 0 ? "\ncanvas zoom: all passed\n" : `\ncanvas zoom: ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
