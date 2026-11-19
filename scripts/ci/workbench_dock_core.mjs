// SPDX-License-Identifier: MIT
//
// The dock adapter's pure core (issue #560, `studio-workbench-layout-v1.md` sections 2.4 and 4.4).
//
//   node scripts/ci/workbench_dock_core.mjs
//
// No browser: it imports `dist-ssr/dock-core.js` and `dist-ssr/workbench.js` (`npm run build` is a prerequisite;
// a missing build is exit code 2) and the real flexlayout-react `Model`/`Actions`, and checks
//
//   1. the round trip model -> library JSON -> real library `Model` -> JSON -> model equals the original (its
//      visible projection: collapsed groups are not shown to the library), for the default, the fixture, every
//      preset and the result of every kind of operation;
//   2. every gesture the library can report maps to one model operation, and a V1-excluded one is refused;
//   3. the splitter arithmetic (aria values, 16 px and 64 px steps, Home and End, clamps).

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const WEB = resolve(REPO, "designer", "web");
let wb;
let dock;
let fl;
try {
  wb = await import(pathToFileURL(resolve(WEB, "dist-ssr", "workbench.js")).href);
  dock = await import(pathToFileURL(resolve(WEB, "dist-ssr", "dock-core.js")).href);
  fl = await import(pathToFileURL(resolve(WEB, "node_modules", "flexlayout-react", "dist", "index.js")).href);
} catch (error) {
  process.stderr.write(
    `workbench_dock_core: build output missing (${String(error)}); run \`npm run build --prefix designer/web\`\n`,
  );
  process.exit(2);
}

let failures = 0;
let passed = 0;
function test(name, fn) {
  try {
    fn();
    passed += 1;
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n  ${String(error.message).split("\n").join("\n  ")}\n`);
  }
}

const dflt = () => wb.freshDefault();
const defs = wb.PANELS;
const fixture = JSON.parse(
  readFileSync(resolve(REPO, "tests", "fixtures", "studio-layout", "v1.json"), "utf8"),
);

function roundTrip(layout, label) {
  const json = dock.toLibrary(layout, { defs });
  const model = fl.Model.fromJson(json);
  const back = dock.fromLibrary(model.toJson(), layout);
  assert.deepEqual(back, dock.visibleLayout(layout), `${label}: model -> library -> model equals the (visible) original`);
  assert.deepEqual(dock.fromLibrary(json, layout), dock.visibleLayout(layout), `${label}: and without the library in between`);
}

test("round trip: default (collapsed bottom group is dropped), fixture, presets", () => {
  roundTrip(dflt(), "default");
  roundTrip(wb.reconcile(fixture), "fixture");
  for (const id of Object.keys(wb.PRESETS)) {
    roundTrip(wb.applyPreset(dflt(), id).layout, `preset ${id}`);
  }
});

test("round trip: an expanded layout is lossless against the original itself", () => {
  const open = wb.collapseGroup(dflt(), "g-bottom", false).layout;
  const json = dock.toLibrary(open, { defs });
  assert.deepEqual(dock.fromLibrary(fl.Model.fromJson(json).toJson(), open), open);
});

test("round trip: after every kind of operation", () => {
  let layout = wb.collapseGroup(dflt(), "g-bottom", false).layout;
  const steps = [
    (l) => wb.movePanel(l, "canvas", { edge: "bottom", of: "g-editor" }).layout,
    (l) => wb.movePanel(l, "diagnostics", { edge: "right", of: "root" }).layout,
    (l) => wb.movePanel(l, "scene", { group: "g-editor", index: 0 }).layout,
    (l) => wb.resizeSplit(l, [], [0.3, 0.7]).layout,
    (l) => wb.closePanel(l, "call-trace").layout,
    (l) => wb.maximize(l, "editor").layout,
    (l) => wb.maximize(l, null).layout,
    (l) => wb.openPanel(l, "call-trace", { group: "g-editor" }).layout,
    (l) => wb.splitGroup(l, "g-editor", "column", "project").layout,
  ];
  for (const [index, step] of steps.entries()) {
    layout = step(layout);
    roundTrip(layout, `after step ${index}`);
  }
});

test("a vertical root and a root that is one group both load", () => {
  const vertical = wb.movePanel(dflt(), "project", { edge: "top", of: "root" }).layout;
  assert.equal(vertical.root.dir, "column");
  assert.equal(dock.toLibrary(vertical, { defs }).global.rootOrientationVertical, true);
  roundTrip(vertical, "vertical root");
  const single = wb.phoneLayout(dflt());
  assert.equal(single.root.type, "group");
  roundTrip(single, "single group root");
});

test("phone: no drag and no maximize in the library JSON", () => {
  const json = dock.toLibrary(wb.phoneLayout(dflt()), { defs, phone: true });
  assert.equal(json.global.tabSetEnableMaximize, false);
  const tabset = json.layout.children[0];
  assert.equal(tabset.enableDrag, false);
  assert.ok(tabset.children.every((tab) => tab.enableDrag === false));
});

test("the library JSON carries our ids: tab = panel id, tabset = group id, rows = their path", () => {
  const json = dock.toLibrary(dflt(), { defs });
  assert.equal(json.layout.id, "row:");
  assert.deepEqual(dock.pathOfRowId("row:1.0"), [1, 0]);
  assert.deepEqual(dock.pathOfRowId("row:"), []);
  assert.equal(dock.pathOfRowId("other"), null);
  assert.equal(json.layout.children[0].id, "g-project");
  assert.equal(json.layout.children[0].children[0].id, "project");
  assert.equal(JSON.stringify(json).includes("g-bottom"), false, "the collapsed group never reaches the library");
});

const active = (layout) => (id) => wb.findGroup(layout, id)?.active ?? null;

test("gestures: select, close, move (tab, edge, root), resize, maximize map to one operation", () => {
  const layout = wb.collapseGroup(dflt(), "g-bottom", false).layout;
  const g = (action) => dock.gestureOf(action, layout, active(layout));
  assert.deepEqual(g(fl.Actions.selectTab("scene")), { kind: "operation", operation: { kind: "activate", panelId: "scene" } });
  assert.deepEqual(g(fl.Actions.deleteTab("canvas")), { kind: "operation", operation: { kind: "close", panelId: "canvas" } });
  assert.deepEqual(g(fl.Actions.moveNode("canvas", "g-editor", fl.DockLocation.CENTER, -1)).operation, {
    kind: "move",
    panelId: "canvas",
    target: { group: "g-editor", index: undefined },
  });
  assert.deepEqual(g(fl.Actions.moveNode("canvas", "g-editor", fl.DockLocation.CENTER, 0)).operation.target, { group: "g-editor", index: 0 });
  assert.deepEqual(g(fl.Actions.moveNode("canvas", "g-editor", fl.DockLocation.BOTTOM, -1)).operation.target, { edge: "bottom", of: "g-editor" });
  assert.deepEqual(g(fl.Actions.moveNode("diagnostics", "row:", fl.DockLocation.RIGHT, -1)).operation.target, { edge: "right", of: "root" });
  assert.equal(g(fl.Actions.moveNode("diagnostics", "row:0", fl.DockLocation.RIGHT, -1)).kind, "refuse", "a nested row is not a drop target");
  assert.deepEqual(g(fl.Actions.adjustWeights("row:", [30, 70])).operation, { kind: "resize", path: [], sizes: [0.3, 0.7], visible: true });
  assert.deepEqual(g(fl.Actions.adjustWeights("row:1", [72, 28])).operation.path, [1]);
  assert.deepEqual(g(fl.Actions.maximizeToggle("g-editor")).operation, { kind: "maximize", panelId: "editor" });
  const maxed = wb.maximize(layout, "editor").layout;
  assert.deepEqual(dock.gestureOf(fl.Actions.maximizeToggle("g-editor"), maxed, active(maxed)).operation, { kind: "maximize", panelId: null });
  assert.equal(g(fl.Actions.setActiveTabset("g-editor")).kind, "pass");
});

test("gestures: popout, float, rename and unknown panels are refused (no floating windows in V1)", () => {
  const layout = dflt();
  for (const action of [fl.Actions.popoutTab("canvas"), fl.Actions.popoutTabset("g-preview"), fl.Actions.renameTab("canvas", "x")]) {
    assert.equal(dock.gestureOf(action, layout, active(layout)).kind, "refuse");
  }
  assert.equal(
    dock.gestureOf(fl.Actions.moveNode("ghost", "g-editor", fl.DockLocation.CENTER, -1), layout, active(layout)).kind,
    "refuse",
  );
});

test("a gesture's operation, applied to the model, equals the library applying the same gesture", () => {
  let layout = wb.collapseGroup(dflt(), "g-bottom", false).layout;
  const model = fl.Model.fromJson(dock.toLibrary(layout, { defs }));
  const gestures = [
    fl.Actions.selectTab("scene"),
    fl.Actions.moveNode("scene", "g-editor", fl.DockLocation.RIGHT, -1),
    fl.Actions.moveNode("diagnostics", "g-preview", fl.DockLocation.CENTER, -1),
    fl.Actions.deleteTab("call-trace"),
  ];
  const strip = (node) =>
    node.type === "group" ? { panels: node.panels, active: node.active } : { dir: node.dir, children: node.children.map(strip) };
  for (const gesture of gestures) {
    const mapped = dock.gestureOf(gesture, layout, active(layout));
    assert.equal(mapped.kind, "operation");
    layout = wb.applyOperation(layout, mapped.operation).layout;
    model.doAction(gesture);
    const fromLibrary = dock.fromLibrary(model.toJson(), layout);
    assert.deepEqual(strip(fromLibrary.root), strip(dock.visibleLayout(layout).root), `after ${gesture.type}`);
  }
});

test("splitter: aria values come from the model, not 0..100", () => {
  assert.deepEqual(dock.separatorValues([0.18, 0.82], 0, [0.1, 0.1]), { now: 18, min: 10, max: 90 });
  assert.deepEqual(dock.separatorValues([0.25, 0.5, 0.25], 1, [0.1, 0.1, 0.1]), { now: 75, min: 35, max: 90 });
  assert.equal(dock.separatorValues([0.5, 0.5], 0, [0.2, 0.2]).max, 80);
});

test("splitter keys: Arrow 16 px, Shift+Arrow 64 px, Home, End, clamped, orientation-aware", () => {
  const min = [0.1, 0.1];
  const sizes = [0.5, 0.5];
  const at = (key, shift, vertical = false) => dock.sizesForKey(key, shift, vertical, sizes, 0, 1000, min);
  assert.deepEqual(at("ArrowRight", false), [0.516, 0.484]);
  assert.deepEqual(at("ArrowLeft", false), [0.484, 0.516]);
  assert.deepEqual(at("ArrowRight", true), [0.564, 0.436]);
  assert.deepEqual(at("Home", false), [0.1, 0.9]);
  assert.deepEqual(at("End", false), [0.9, 0.1]);
  assert.equal(at("ArrowDown", false), null, "a horizontal splitter ignores the vertical arrows");
  assert.deepEqual(at("ArrowDown", true, true), [0.564, 0.436]);
  assert.equal(at("a", false), null);
  assert.deepEqual(
    dock.sizesForKey("ArrowRight", true, false, [0.89, 0.11], 0, 1000, min),
    [0.9, 0.1],
    "clamped at the neighbour's minimum",
  );
  assert.equal(dock.sizesForKey("ArrowRight", false, false, sizes, 0, 0, min), null, "no extent, no move");
});

test("splitter path: library data-layout-path to a model path and index", () => {
  assert.deepEqual(dock.parseSplitterPath("/s0"), { path: [], index: 0 });
  assert.deepEqual(dock.parseSplitterPath("/r1/s0"), { path: [1], index: 0 });
  assert.deepEqual(dock.parseSplitterPath("/r1/r0/s2"), { path: [1, 0], index: 2 });
  assert.equal(dock.parseSplitterPath("/ts0"), null);
  assert.equal(dock.parseSplitterPath(null), null);
  const split = dock.splitAtPath(wb.collapseGroup(dflt(), "g-bottom", false).layout, [1]);
  assert.equal(split.dir, "column");
  assert.equal(dock.splitAtPath(dflt(), [1]), null, "with the panel area collapsed the second child is a group, not a split");
});

process.stdout.write(`workbench_dock_core: ${passed} passed, ${failures} failed\n`);
process.exit(failures === 0 ? 0 : 1);
