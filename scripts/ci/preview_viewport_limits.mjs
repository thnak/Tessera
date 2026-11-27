// SPDX-License-Identifier: MIT
//
// The Canvas viewport limits (issue #690): the page's copy of the clamp the engine applies, tested
// against the module the page ships.
//
// **Why two copies and two tests.** The page sizes the canvas backing store itself
// (`sizeToDevicePixels`), so a size the engine would clamp still reaches the browser unclamped if only
// the module knew the rule - and the browser allocates that surface before the module sees a number.
// The engine's copy is a pure function in `tools/codegen/canvas_viewport_request.cpp` with its own
// test (`tests/unit/canvas_viewport_limit_tests.cpp`); this file asserts the shipped page module, and
// the table below restates the engine's numbers so a change on either side fails the other. That
// coupling is deliberate: a clamp that disagrees between the two halves is a device the page believes
// it sized and the engine then lays out differently.
//
//   node scripts/ci/preview_viewport_limits.mjs
//
// Needs the SSR build (`npm run build:chrome --prefix designer/web`). No browser: the clamp is pure
// arithmetic, and what the pane shows when it fires is covered by the canvas pane-state checks.

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const MODULE = resolve(HERE, "..", "..", "designer", "web", "dist-ssr", "canvas-viewport.js");

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

let viewport;
try {
  viewport = await import(pathToFileURL(MODULE).href);
} catch {
  process.stderr.write(
    `SETUP ERROR: ${MODULE} is missing. Build the page first:\n` +
      "  npm ci --prefix designer/web && npm run build:chrome --prefix designer/web\n",
  );
  process.exit(2);
}

const { VIEWPORT_LIMITS, VIEWPORT_WIDTH, VIEWPORT_HEIGHT, clampViewport, requestedViewport, sizeToDevicePixels, backingBytes } =
  viewport;

// The engine's numbers, restated (`tools/codegen/canvas_viewport_request.hpp`). A change to either
// side alone fails here or in the engine's test, which is the drift this table exists to catch.
const ENGINE = { minExtent: 1, maxExtent: 8192, minDpr: 0.01, maxDpr: 4, maxBackingBytes: 256 * 1024 * 1024 };

/** A stand-in for the DOM's canvas: exactly the members `sizeToDevicePixels` writes. */
function fakeCanvas(initial = { width: 0, height: 0 }) {
  return { width: initial.width, height: initial.height, style: { width: "", height: "" } };
}

function near(actual, wanted, message = "") {
  assert.ok(
    Math.abs(actual - wanted) <= 1e-3,
    `${message}${message ? ": " : ""}got ${actual}, wanted ${wanted}`,
  );
}

test("the page's limits are the engine's numbers", () => {
  assert.deepEqual({ ...VIEWPORT_LIMITS }, ENGINE);
});

test("an ordinary viewport is passed through unclamped", () => {
  const result = clampViewport({ width: 390, height: 844, dpr: 2 });
  assert.deepEqual(result, { width: 390, height: 844, dpr: 2, clamped: false });
});

test("the limits themselves are legal", () => {
  const result = clampViewport({ width: ENGINE.maxExtent, height: ENGINE.maxExtent, dpr: ENGINE.maxDpr });
  // 8192 x 8192 x 4 x 4 is 1 GiB, so the byte cap reduces the DPR even though each field is legal.
  assert.ok(result.clamped, "the byte cap clamps the largest legal rectangle");
  near(result.width, ENGINE.maxExtent);
  near(result.height, ENGINE.maxExtent);
  assert.ok(backingBytes(result) <= ENGINE.maxBackingBytes * 1.001, "the store fits the cap");
});

test("a huge extent clamps instead of failing", () => {
  const result = clampViewport({ width: 100000, height: 200000, dpr: 1 });
  near(result.width, ENGINE.maxExtent);
  near(result.height, ENGINE.maxExtent);
  assert.equal(result.clamped, true);
});

test("a high DPR clamps the scale only, never the layout", () => {
  const result = clampViewport({ width: 320, height: 240, dpr: 100 });
  near(result.width, 320, "the layout width is untouched");
  near(result.height, 240, "the layout height is untouched");
  near(result.dpr, ENGINE.maxDpr);
  assert.equal(result.clamped, true);
});

test("the byte cap reduces the DPR, not the extent", () => {
  const result = clampViewport({ width: 4096, height: 4096, dpr: 4 });
  near(result.width, 4096, "the layout width is kept");
  near(result.height, 4096, "the layout height is kept");
  assert.ok(result.dpr < 4, "the DPR was reduced");
  assert.ok(backingBytes(result) <= ENGINE.maxBackingBytes * 1.001, "the reduced store fits the cap");
  assert.equal(result.clamped, true);
});

test("zero and negative values are refused, never clamped", () => {
  assert.equal(clampViewport({ width: 0, height: 240, dpr: 1 }), null);
  assert.equal(clampViewport({ width: 320, height: -1, dpr: 1 }), null);
  assert.equal(clampViewport({ width: 320, height: 240, dpr: 0 }), null);
  assert.equal(clampViewport({ width: -320, height: -240, dpr: -1 }), null);
});

test("non-finite values are refused", () => {
  assert.equal(clampViewport({ width: NaN, height: 240, dpr: 1 }), null);
  assert.equal(clampViewport({ width: 320, height: Infinity, dpr: 1 }), null);
  assert.equal(clampViewport({ width: 320, height: 240, dpr: NaN }), null);
  assert.equal(clampViewport(null), null);
  assert.equal(clampViewport(undefined), null);
});

test("the minimum is a real device", () => {
  const result = clampViewport({ width: ENGINE.minExtent, height: ENGINE.minExtent, dpr: ENGINE.minDpr });
  assert.deepEqual(result, { width: 1, height: 1, dpr: 0.01, clamped: false });
});

test("a missing toolbar reading falls back to the default device, not to nothing", () => {
  assert.deepEqual(requestedViewport(() => null), { width: VIEWPORT_WIDTH, height: VIEWPORT_HEIGHT, dpr: 1, clamped: false });
  assert.deepEqual(requestedViewport(() => undefined), { width: VIEWPORT_WIDTH, height: VIEWPORT_HEIGHT, dpr: 1, clamped: false });
});

test("an over-limit toolbar reading is clamped, not defaulted", () => {
  const result = requestedViewport(() => ({ width: 50000, height: 50000, dpr: 8 }));
  assert.equal(result.clamped, true);
  near(result.width, ENGINE.maxExtent);
});

test("the backing store is never sized past the limits", () => {
  const canvas = fakeCanvas();
  const sized = sizeToDevicePixels(canvas, { width: 4096, height: 4096, dpr: 4 });
  assert.ok(backingBytes({ width: sized.width / sized.dpr, height: sized.height / sized.dpr, dpr: sized.dpr }) <= ENGINE.maxBackingBytes * 1.001);
  assert.ok(canvas.width <= Math.ceil(ENGINE.maxExtent * ENGINE.maxDpr));
  assert.equal(sized.clamped, true, "the sizing reports the clamp so the pane can name it");
});

test("a legal viewport sizes the CSS box to the logical rectangle", () => {
  const canvas = fakeCanvas();
  const sized = sizeToDevicePixels(canvas, { width: 390, height: 844, dpr: 2 });
  assert.equal(canvas.width, 780);
  assert.equal(canvas.height, 1688);
  assert.equal(canvas.style.width, "390px");
  assert.equal(canvas.style.height, "844px");
  assert.equal(sized.clamped, false);
});

test("the clamp is idempotent", () => {
  const once = clampViewport({ width: 4096, height: 4096, dpr: 4 });
  const twice = clampViewport(once);
  assert.deepEqual({ ...twice, clamped: true }, { ...once, clamped: true });
});

process.stdout.write(
  failures === 0
    ? `\npreview_viewport_limits: all checks passed\n`
    : `\npreview_viewport_limits: ${failures} check(s) failed\n`,
);
process.exit(failures === 0 ? 0 : 1);
