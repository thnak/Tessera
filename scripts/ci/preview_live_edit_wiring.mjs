// SPDX-License-Identifier: MIT
//
// The live edit applies instead of restarting (issue #865).
//
//   node scripts/ci/preview_live_edit_wiring.mjs
//
// Plain Node, no browser. Regression for "an edit to an open Canvas view does not repaint": the
// controller (`page-live.ts`) is created once, and the page used to hand it a file list captured at that
// moment. Since #391 that list is empty, so every keystroke in an opened project took the restart branch
// (state reset, frame counter back to #1) and never the apply. The page now hands it
// `liveEditKindOf(() => filesRef.current)`; this drives the real controller with that reader against a
// project that is opened AFTER the controller exists.

import assert from "node:assert/strict";

import { liveEditKindOf } from "../../designer/web/live-edit-kind.js";
import { createLiveEdit, APPLY_DEBOUNCE_MS } from "../../designer/web/src/page-live.ts";

let failures = 0;
async function test(name, body) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error?.stack ?? error}\n`);
  }
}

// A manual clock: the debounce is driven, not waited for.
function clock() {
  const pending = new Map();
  let next = 1;
  return {
    setTimeout: (fn) => (pending.set(next, fn), next++),
    clearTimeout: (id) => void pending.delete(id),
    fire: async () => {
      for (const [id, fn] of [...pending]) {
        pending.delete(id);
        fn();
      }
      await new Promise((resolve) => setImmediate(resolve));
    },
    size: () => pending.size,
  };
}

function harness() {
  // The page boots with no project (#391); a project is opened later through the same ref.
  const box = { files: [], active: "" };
  const calls = { apply: 0, run: 0 };
  const session = { apply: async () => void calls.apply++, run: async () => void calls.run++ };
  const timers = clock();
  const live = createLiveEdit(
    {
      sessions: () => ({ canvas: session }),
      currentView: () => "canvas",
      editedFile: () => box.active,
      kindOf: liveEditKindOf(() => box.files),
    },
    timers,
  );
  return { box, calls, live, timers };
}

await test("a keystroke in a source file of a project opened after boot applies, never restarts", async () => {
  const { box, calls, live, timers } = harness();
  box.files = [
    { path: "ui/App.tsx", kind: "source" },
    { path: "styles/theme.css", kind: "stylesheet" },
  ];
  box.active = "ui/App.tsx";
  live.schedule();
  assert.equal(timers.size(), 1, "the edit is debounced, not restarted at once");
  await timers.fire();
  assert.deepEqual(calls, { apply: 1, run: 0 });
});

await test("a burst is one apply", async () => {
  const { box, calls, live, timers } = harness();
  box.files = [{ path: "ui/App.tsx", kind: "source" }];
  box.active = "ui/App.tsx";
  for (let i = 0; i < 5; i += 1) {
    live.schedule();
  }
  await timers.fire();
  assert.deepEqual(calls, { apply: 1, run: 0 });
  assert.ok(APPLY_DEBOUNCE_MS > 0);
});

await test("a stylesheet edit applies; the config restarts", async () => {
  const { box, calls, live, timers } = harness();
  box.files = [
    { path: "styles/theme.css", kind: "stylesheet" },
    { path: "tessera.json", kind: "config" },
  ];
  box.active = "styles/theme.css";
  live.schedule();
  await timers.fire();
  assert.deepEqual(calls, { apply: 1, run: 0 });
  box.active = "tessera.json";
  live.schedule();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(calls.run, 1);
});

await test("the reader sees the files at call time, and an unknown path is other", () => {
  const box = { files: [] };
  const kindOf = liveEditKindOf(() => box.files);
  assert.equal(kindOf("ui/App.tsx"), "other");
  box.files = [{ path: "ui/App.tsx", kind: "source" }];
  assert.equal(kindOf("ui/App.tsx"), "source");
});

process.stdout.write(failures === 0 ? "\npreview_live_edit_wiring: all cases passed\n" : `\n${failures} failed\n`);
process.exit(failures > 0 ? 1 : 0);
