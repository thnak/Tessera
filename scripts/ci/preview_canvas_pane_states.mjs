// SPDX-License-Identifier: MIT
//
// The canvas pane never goes silently blank (owner report: Studio canvas "renders completely black" and
// "does not change size with the Device").
//
//   node scripts/ci/preview_canvas_pane_states.mjs
//
// Root cause of that symptom: when a sample did not apply (an unresolved import, a tree the engine cannot
// preview), the session deliberately keeps the last frame and only wrote one amber line into the status
// strip. Every Device/viewport change re-ran the same failing apply, so the frame never changed, and a
// first apply that failed left a blank black surface. The pane now says it in the pane itself:
//
//   * a source that does not apply shows the "Source not applied" box with the compiler's reason;
//   * an applied source that paints nothing shows the "Nothing to draw" box;
//   * a later successful apply clears the not-applied box and nothing else.
//
// Needs the SSR build (`npm run build --prefix designer/web`), like the studio smoke.

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const MODULE = resolve(HERE, "..", "..", "designer", "web", "dist-ssr", "canvas-pane.js");

let failures = 0;
async function test(name, body) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

let canvasPane;
try {
  canvasPane = await import(pathToFileURL(MODULE).href);
} catch {
  process.stderr.write(
    `SETUP ERROR: ${MODULE} is missing. Build it first:\n` +
      "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
  );
  process.exit(2);
}

function element(extra = {}) {
  return { hidden: true, textContent: "", dataset: {}, title: "", ...extra };
}

function fixture() {
  const heading = element({ textContent: "Preview failed" });
  const elements = {
    status: element(),
    unsupported: element({ querySelector: () => null }),
    unsupportedBody: element(),
    frame: element(),
    canvas: element(),
    failed: element({ querySelector: (selector) => (selector === "#failed-heading" ? heading : null) }),
    failedBody: element(),
    frameStats: element(),
    statFrame: element(),
    statDraws: element(),
    statVertices: element(),
    statNodes: element(),
    statDpr: element(),
    statFont: element(),
    stateRow: element(),
    statState: element(),
    statusFrame: null,
    pickOverlay: null,
    inspectPanel: null,
  };
  const statuses = [];
  const pane = canvasPane.createCanvasPane(elements, (text, kind) => statuses.push([text, kind]));
  return { pane, elements, heading, statuses };
}

const report = (extra) => ({
  hasDevice: true,
  width: 320,
  height: 240,
  drawCalls: 6,
  vertices: 138,
  nodes: 5,
  error: "",
  ...extra,
});

await test("a source that does not apply says so in the pane, with the reason", () => {
  const { pane, elements, heading, statuses } = fixture();
  pane.showNotApplied("TESSERA_IR_UNRESOLVED: import `./Button` does not name a file of this project");
  assert.equal(elements.failed.hidden, false, "the box is visible");
  assert.equal(heading.textContent, "Source not applied");
  assert.match(elements.failedBody.textContent, /does not name a file of this project/);
  assert.match(elements.failedBody.textContent, /last frame that did apply/);
  assert.match(statuses.at(-1)[0], /^source not applied: /);
  assert.equal(statuses.at(-1)[1], "warning");
});

await test("the last good frame is not hidden by a failed apply (issue #444)", () => {
  const { pane, elements } = fixture();
  elements.frame.hidden = false;
  pane.showNotApplied("boom");
  assert.equal(elements.frame.hidden, false);
});

await test("a successful apply clears the not-applied box and restores the heading", () => {
  const { pane, elements, heading } = fixture();
  pane.showNotApplied("boom");
  pane.clearNotApplied();
  assert.equal(elements.failed.hidden, true);
  assert.equal(heading.textContent, "Preview failed");
  assert.equal(elements.failedBody.textContent, "");
});

await test("clearing does not touch a box that is not the not-applied message", () => {
  const { pane, elements } = fixture();
  pane.showFailed("the module died");
  pane.clearNotApplied();
  assert.equal(elements.failed.hidden, false);
  assert.equal(elements.failedBody.textContent, "the module died");
});

await test("an applied source that paints nothing shows Nothing to draw, not a bare black canvas", () => {
  const { pane, elements, heading, statuses } = fixture();
  pane.renderReport(report({ drawCalls: 0, vertices: 0, nodes: 0 }));
  assert.equal(elements.failed.hidden, false);
  assert.equal(heading.textContent, "Nothing to draw");
  assert.match(elements.failedBody.textContent, /painted no boxes/);
  assert.match(statuses.at(-1)[0], /no draw calls/);
});

await test("a frame that draws hides the Nothing to draw box again", () => {
  const { pane, elements, heading } = fixture();
  pane.renderReport(report({ drawCalls: 0 }));
  pane.renderReport(report());
  assert.equal(elements.failed.hidden, true);
  assert.equal(heading.textContent, "Preview failed");
  assert.equal(elements.statDraws.textContent, "6");
});

process.stdout.write(
  `\n${failures === 0 ? "canvas pane states: all cases passed" : `${failures} failed`}\n`,
);
if (failures > 0) {
  process.exitCode = 1;
}
