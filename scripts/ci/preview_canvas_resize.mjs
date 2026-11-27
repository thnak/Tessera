// SPDX-License-Identifier: MIT
//
// The canvas drag-resize arithmetic (issue #690 slice b), tested against the module the page ships.
//
//   node scripts/ci/preview_canvas_resize.mjs
//
// Needs only the SSR build (`npm run build:chrome --prefix designer/web`): the arithmetic is pure, the
// DOM half (handles, pointer capture, the readout) is `preview_browser_canvas_size.mjs`'s neighbour and
// a browser gate. What is worth pinning here is the two rules the gesture exists to keep:
//
//   1. A drag is a burst, and the engine sees one size per animation frame, not one per pointer event -
//      and exactly one settled size on pointer-up.
//   2. The size a drag reaches is held to the same limits as any other viewport request, because it
//      goes through the same `clampViewport` (`canvas-viewport.ts`) the engine call does.

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const MODULE = resolve(HERE, "..", "..", "designer", "web", "dist-ssr", "canvas-resize.js");

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

let resize;
try {
  resize = await import(pathToFileURL(MODULE).href);
} catch {
  process.stderr.write(
    `SETUP ERROR: ${MODULE} is missing. Build the page first:\n` +
      "  npm ci --prefix designer/web && npm run build:chrome --prefix designer/web\n",
  );
  process.exit(2);
}

const { draggedSize, draggedViewport, shouldStep, resizeReadout, axisOf } = resize;

test("a handle drags its own axis", () => {
  assert.equal(axisOf("right"), "width");
  assert.equal(axisOf("bottom"), "height");
  assert.equal(axisOf("corner"), "both");
});

test("an edge drag moves only its own axis", () => {
  const start = { width: 320, height: 240 };
  const right = draggedSize(start, "right", { x: 100, y: 999 }, 1);
  assert.deepEqual(right, { width: 420, height: 240 });
  const bottom = draggedSize(start, "bottom", { x: 999, y: 100 }, 1);
  assert.deepEqual(bottom, { width: 320, height: 340 });
});

test("the corner moves both axes", () => {
  const got = draggedSize({ width: 320, height: 240 }, "corner", { x: 40, y: -40 }, 1);
  assert.deepEqual(got, { width: 360, height: 200 });
});

test("the pointer's travel is divided by the pane's zoom", () => {
  // The canvas' CSS box is the LOGICAL viewport, so at zoom 1 one CSS pixel of travel is one logical
  // pixel however many device pixels it covers: the DPR is not a divisor. A pane at 68% (a zoomed-out
  // preview) means 100 screen pixels are ~147 logical ones.
  const unzoomed = draggedSize({ width: 320, height: 240 }, "right", { x: 100, y: 0 }, 1);
  assert.deepEqual(unzoomed, { width: 420, height: 240 });
  const zoomedOut = draggedSize({ width: 320, height: 240 }, "right", { x: 100, y: 0 }, 68 / 100);
  assert.deepEqual(zoomedOut, { width: 467, height: 240 }, "100 screen px at 68% is ~147 logical px");
});

test("the device pixel ratio does not scale a drag", () => {
  // The bug the browser gate caught: a drag divided by the DPR moved half as far at DPR 2.
  const a = draggedViewport({ width: 320, height: 240, dpr: 1 }, "corner", { x: 100, y: 60 });
  const b = draggedViewport({ width: 320, height: 240, dpr: 2 }, "corner", { x: 100, y: 60 });
  assert.equal(a.width, b.width, "the same travel reaches the same logical size at any DPR");
  assert.equal(a.height, b.height);
});

test("a bad ratio cannot collapse a drag", () => {
  for (const bad of [0, -1, NaN, Infinity]) {
    const got = draggedSize({ width: 320, height: 240 }, "right", { x: 10, y: 0 }, bad);
    assert.equal(got.width, 330, `ratio ${bad} is treated as 1`);
  }
});

test("a drag cannot go below the minimum extent", () => {
  const got = draggedSize({ width: 320, height: 240 }, "corner", { x: -10000, y: -10000 }, 1);
  assert.deepEqual(got, { width: 1, height: 1 });
});

test("a drag past a limit is clamped, and the readout says so", () => {
  const got = draggedViewport({ width: 320, height: 240, dpr: 1 }, "right", { x: 100000, y: 0 });
  assert.equal(got.width, 8192, "the extent maximum holds for a drag too");
  assert.equal(got.clamped, true);
  assert.match(resizeReadout(got), /clamped/);
});

test("an ordinary drag is not reported as clamped", () => {
  const got = draggedViewport({ width: 320, height: 240, dpr: 1 }, "right", { x: 100, y: 0 });
  assert.deepEqual(got, { width: 420, height: 240, dpr: 1, clamped: false });
  assert.equal(resizeReadout(got), "420x240");
});

test("a drag is one step per animation frame, not one per pointer event", () => {
  let previous = { size: null, atMs: 0 };
  let size = { width: 320, height: 240 };
  let steps = 0;
  // Five pointer events inside one frame: the first opens the drag, the rest are the same frame.
  for (let i = 0; i < 5; i += 1) {
    size = { width: 320 + (i + 1) * 10, height: 240 };
    if (shouldStep(previous, { size, atMs: 0 }, false)) {
      steps += 1;
      previous = { size, atMs: 0 };
    }
  }
  assert.equal(steps, 1, `five moves in one frame cost one step, got ${steps}`);
});

test("a move that lands on the same rounded size costs nothing", () => {
  const previous = { size: { width: 420, height: 240 }, atMs: 100 };
  assert.equal(shouldStep(previous, { size: { width: 420, height: 240 }, atMs: 200 }, false), false);
});

test("the next frame is allowed after a frame has elapsed", () => {
  const previous = { size: { width: 420, height: 240 }, atMs: 100 };
  assert.equal(shouldStep(previous, { size: { width: 430, height: 240 }, atMs: 120 }, false), true);
});

test("pointer-up always takes one settled step", () => {
  const previous = { size: { width: 420, height: 240 }, atMs: 100 };
  // Same size, same millisecond: the settled step still runs, and the engine answers changed:false.
  assert.equal(shouldStep(previous, { size: { width: 420, height: 240 }, atMs: 100 }, true), true);
});

test("the readout is the number the engine will get", () => {
  // The readout is computed from the same clamp, so it cannot disagree with the call.
  const got = draggedViewport({ width: 4096, height: 4096, dpr: 4 }, "corner", { x: 0, y: 0 });
  assert.equal(resizeReadout(got), "4096x4096 (clamped)", "the byte cap shows in the readout");
});

test("a size no call can take shows no size rather than a number", () => {
  assert.equal(resizeReadout(null), "no size");
});

process.stdout.write(
  failures === 0
    ? `\npreview_canvas_resize: all checks passed\n`
    : `\npreview_canvas_resize: ${failures} check(s) failed\n`,
);
process.exit(failures === 0 ? 0 : 1);
