// SPDX-License-Identifier: MIT
//
// The canvas pane's animation frame loop (issue #603), tested against the module the page ships.
//
//   node scripts/ci/preview_canvas_motion.mjs
//
// Needs only the SSR build (`npm run build:chrome --prefix designer/web`). The engine's half - that a
// transition's values advance with the page's timestamps, that the keep-alive is false once nothing
// runs, and that reduced motion settles at once - is gated in C++ with a manual clock
// (`scene_canvas_motion_tests`). What is gated here is the page's half of the same contract, against a
// small model of the engine that follows the same rule (animating while time remains):
//
//   1. While the engine reports `animating`, the page draws one frame per display refresh with the
//      refresh's own timestamp, and the values it paints advance.
//   2. The refresh after the engine says it is done requests nothing: an idle canvas presents zero
//      frames, however long it sits.
//   3. Two sources of frames (an input-driven one and the animation chain) never keep two chains alive
//      and never present twice for one refresh.
//   4. A failing frame ends the chain instead of retrying forever.

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const MODULE = resolve(HERE, "..", "..", "designer", "web", "dist-ssr", "canvas-motion.js");

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

let motion;
try {
  motion = await import(pathToFileURL(MODULE).href);
} catch {
  process.stderr.write(
    `SETUP ERROR: ${MODULE} is missing. Build the page first:\n` +
      "  npm ci --prefix designer/web && npm run build:chrome --prefix designer/web\n",
  );
  process.exit(2);
}

const { createMotionLoop, prefersReducedMotion, parseTickReport } = motion;

// A display: `requestAnimationFrame` callbacks queue up and run together on the next refresh, all with
// that refresh's timestamp, exactly as a browser delivers them.
function display() {
  const queue = [];
  let now = 1000;
  return {
    request(callback) {
      queue.push(callback);
    },
    get queued() {
      return queue.length;
    },
    refresh() {
      now += 1000 / 60;
      const callbacks = queue.splice(0, queue.length);
      for (const callback of callbacks) {
        callback(now);
      }
      return now;
    },
  };
}

// The engine, reduced to the rule the C++ tests pin: a transition of `durationMs` is animating until its
// elapsed time reaches it, and the value moves linearly with the timestamps it is given.
function engine(durationMs) {
  const state = { startedAt: null, value: 0, frames: 0, ticks: [] };
  state.frame = (nowMs) => {
    state.frames += 1;
    state.ticks.push(nowMs);
    if (state.startedAt === null) {
      state.startedAt = nowMs;
    }
    const elapsed = Math.min(nowMs - state.startedAt, durationMs);
    state.value = elapsed / durationMs;
    return { animating: elapsed < durationMs };
  };
  return state;
}

test("a transition draws a frame per refresh, its values advance, and then it stops", () => {
  const screen = display();
  const model = engine(200);
  const loop = motion.createMotionLoop({
    request: (callback) => screen.request(callback),
    frame: (nowMs) => model.frame(nowMs),
    pending: () => false,
  });

  // The input-driven frame that started the transition: the engine says it animates.
  const first = model.frame(screen.refresh());
  loop.keepAlive(first.animating);
  assert.equal(loop.scheduled(), true, "an animating engine keeps the chain alive");

  const values = [model.value];
  let guard = 0;
  while (screen.queued > 0 && guard < 1000) {
    screen.refresh();
    values.push(model.value);
    guard += 1;
  }
  assert.ok(guard < 1000, "the chain terminated");
  for (let index = 1; index < values.length; index += 1) {
    assert.ok(values[index] >= values[index - 1], "values never go backwards");
  }
  assert.ok(values.some((value) => value > 0 && value < 1), "a mid-transition value was painted");
  assert.equal(values.at(-1), 1, "it ends at the final value");
  // 200 ms at 60 Hz is 12 refreshes after the first frame.
  assert.ok(model.frames >= 12 && model.frames <= 14, `frames advance per refresh, got ${model.frames}`);
  assert.equal(loop.scheduled(), false, "the chain stopped");
  assert.equal(screen.queued, 0, "nothing is queued once the transition ended");
});

test("an idle canvas requests no frame, however many refreshes pass", () => {
  const screen = display();
  const model = engine(200);
  const loop = createMotionLoop({
    request: (callback) => screen.request(callback),
    frame: (nowMs) => model.frame(nowMs),
    pending: () => false,
  });
  loop.keepAlive(false); // a draw whose report said `animating:false`
  assert.equal(loop.scheduled(), false);
  for (let index = 0; index < 120; index += 1) {
    screen.refresh();
  }
  assert.equal(model.frames, 0, "no frame was drawn for two seconds of refreshes");
  assert.equal(loop.drawn(), 0);
  assert.equal(screen.queued, 0);
});

test("a repeated keep-alive keeps one chain, not two", () => {
  const screen = display();
  const model = engine(1000);
  const loop = createMotionLoop({
    request: (callback) => screen.request(callback),
    frame: (nowMs) => model.frame(nowMs),
    pending: () => false,
  });
  loop.keepAlive(true);
  loop.keepAlive(true);
  loop.keepAlive(true);
  assert.equal(screen.queued, 1, "three keep-alives queue one frame");
  screen.refresh();
  assert.equal(model.frames, 1, "one frame per refresh");
  assert.equal(screen.queued, 1, "and it re-armed itself once");
});

test("a frame an input already queued wins the refresh", () => {
  const screen = display();
  const model = engine(1000);
  let inputFrameQueued = true;
  const loop = createMotionLoop({
    request: (callback) => screen.request(callback),
    frame: (nowMs) => model.frame(nowMs),
    pending: () => inputFrameQueued,
  });
  loop.keepAlive(true);
  screen.refresh();
  assert.equal(model.frames, 0, "the chain yields instead of presenting twice");
  assert.equal(loop.scheduled(), false, "the chain ended; the input frame's report re-arms it");
  inputFrameQueued = false;
  loop.keepAlive(true); // the input-driven frame's report says it still animates
  screen.refresh();
  assert.equal(model.frames, 1, "the re-armed chain draws");
});

test("a failing frame ends the chain", () => {
  const screen = display();
  let calls = 0;
  const loop = createMotionLoop({
    request: (callback) => screen.request(callback),
    frame: () => {
      calls += 1;
      throw new Error("device lost");
    },
    pending: () => false,
  });
  loop.keepAlive(true);
  screen.refresh();
  screen.refresh();
  screen.refresh();
  assert.equal(calls, 1, "a failed frame is not retried every refresh");
  assert.equal(loop.scheduled(), false);
  assert.equal(screen.queued, 0);
});

test("a frame the page cannot draw (hidden, no device) ends the chain", () => {
  const screen = display();
  const loop = createMotionLoop({
    request: (callback) => screen.request(callback),
    frame: () => null,
    pending: () => false,
  });
  loop.keepAlive(true);
  screen.refresh();
  assert.equal(loop.scheduled(), false);
  assert.equal(screen.queued, 0);
});

test("prefers-reduced-motion is read from the media query, and absent means motion is allowed", () => {
  assert.equal(prefersReducedMotion(undefined), false);
  const seen = [];
  assert.equal(
    prefersReducedMotion((query) => {
      seen.push(query);
      return { matches: true };
    }),
    true,
  );
  assert.deepEqual(seen, ["(prefers-reduced-motion: reduce)"]);
  assert.equal(prefersReducedMotion(() => ({ matches: false })), false);
});

test("the engine's tick report parses and defaults", () => {
  const report = parseTickReport(
    '{"tickVersion":1,"running":true,"dtMs":16.667,"samples":2,"reducedMotion":false}',
  );
  assert.deepEqual(report, { running: true, dtMs: 16.667, samples: 2, reducedMotion: false });
  assert.deepEqual(parseTickReport("{}"), { running: false, dtMs: 0, samples: 0, reducedMotion: false });
});

if (failures !== 0) {
  process.stderr.write(`${failures} failure(s)\n`);
  process.exit(1);
}
process.stdout.write("preview_canvas_motion: ok\n");
