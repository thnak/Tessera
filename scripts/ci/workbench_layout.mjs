// SPDX-License-Identifier: MIT
//
// The workbench layout model's gate (issue #556, `studio-workbench-layout-v1.md` sections 1 to 3).
//
//   node scripts/ci/workbench_layout.mjs
//
// Pure Node, no browser: the model, operations, migration, reconciliation, phone projection and persistence
// are imported from `dist-ssr/workbench.js` (the code the page ships; `npm run build` is a prerequisite and a
// missing build is exit code 2). The dock library is not involved; `scripts/ci/workbench_dock.mjs` (W5) is the
// browser half.

import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const WEB = resolve(REPO, "designer", "web");
const BUNDLE = resolve(WEB, "dist-ssr", "workbench.js");

let wb;
try {
  wb = await import(pathToFileURL(BUNDLE).href);
} catch {
  process.stderr.write(`workbench_layout: ${BUNDLE} is missing; run \`npm run build --prefix designer/web\` first\n`);
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

const clone = (value) => JSON.parse(JSON.stringify(value));
const dflt = () => wb.freshDefault();
const rules = (doc) => {
  const result = wb.validateLayout(doc);
  return result.ok ? [] : result.errors.map((error) => error.rule);
};
const groupIds = (layout) => wb.groupsOf(layout.root).map((group) => group.id);
const fixture = (name) => JSON.parse(readFileSync(resolve(REPO, "tests", "fixtures", "studio-layout", name), "utf8"));
const assertValid = (layout, label) => {
  const result = wb.validateLayout(layout);
  assert.ok(result.ok, `${label}: ${result.ok ? "" : JSON.stringify(result.errors)}`);
};
const group = (id, panels, active = panels[0]) => ({ type: "group", id, panels, active });
const split = (dir, sizes, children) => ({ type: "split", dir, sizes, children });

// ---- 1. the default layout and the panel definitions ------------------------------------------------------

test("the default layout is valid, collapsed at g-bottom, and reconcile leaves it unchanged", () => {
  const layout = dflt();
  assertValid(layout, "default");
  assert.deepEqual(layout.collapsed, ["g-bottom"]);
  assert.deepEqual(wb.reconcile(layout), layout);
  assert.deepEqual(wb.groupOfPanel(layout, "diagnostics").id, "g-bottom");
});

test("panels.json: ids are unique, kebab, and every group's default location resolves in the default layout", () => {
  const ids = wb.PANELS.map((panel) => panel.id);
  assert.equal(new Set(ids).size, ids.length);
  for (const def of wb.PANELS.filter((panel) => !panel.reserved)) {
    assert.match(def.id, /^[a-z][a-z0-9-]*$/);
    assert.ok(wb.groupOfPanel(dflt(), def.id) !== null, `${def.id} is placed in the default layout`);
  }
  assert.equal(wb.PANELS.find((panel) => panel.id === "editor").closable, false);
});

test("every preset is valid after reconcile", () => {
  for (const id of Object.keys(wb.PRESETS)) {
    assertValid(wb.applyPreset(dflt(), id).layout, `preset ${id}`);
  }
});

// ---- 2. the validator rejects every rule of spec 1.3 -------------------------------------------------------

test("validator: format, version, shape", () => {
  assert.ok(rules(null).includes("document"));
  assert.ok(rules({ ...dflt(), format: "x" }).includes("format"));
  assert.ok(rules({ ...dflt(), version: 0 }).includes("version"));
  assert.ok(rules({ ...dflt(), version: 1.5 }).includes("version"));
  assert.ok(rules({ ...dflt(), root: { type: "tree" } }).includes("node-type"));
});

test("validator: sizes sum to 1, one per child, positive; a split has two children", () => {
  const bad = dflt();
  bad.root.sizes = [0.5, 0.6];
  assert.ok(rules(bad).includes("sizes-sum"));
  bad.root.sizes = [1];
  assert.ok(rules(bad).includes("sizes-shape"));
  const lone = dflt();
  lone.root = split("row", [1], [group("g-a", ["editor"])]);
  assert.ok(rules(lone).includes("split-children"));
  const within = dflt();
  within.root.sizes = [0.18 + 5e-7, 0.82];
  assert.deepEqual(rules(within), [], "a sum off by 5e-7 is within the 1e-6 tolerance");
});

test("validator: no same-direction nested split", () => {
  const nested = dflt();
  nested.root = split("row", [0.5, 0.5], [
    group("g-a", ["project"]),
    split("row", [0.5, 0.5], [group("g-b", ["editor"]), group("g-c", ["canvas", "scene"])]),
  ]);
  nested.collapsed = [];
  nested.closed = [];
  assert.deepEqual(rules(nested), ["split-same-dir"]);
});

test("validator: each panel at most once across groups and closed", () => {
  const twice = dflt();
  twice.root.children[1].children[1].panels.push("editor");
  assert.ok(rules(twice).includes("panel-once"));
  const closedToo = dflt();
  closedToo.closed = ["canvas"];
  assert.ok(rules(closedToo).includes("panel-once"));
});

test("validator: groups are non-empty with a valid active panel and unique g-<slug> ids", () => {
  const empty = dflt();
  empty.root.children[0].panels = [];
  assert.ok(rules(empty).includes("group-empty"));
  const active = dflt();
  active.root.children[0].active = "canvas";
  assert.ok(rules(active).includes("group-active"));
  const dup = dflt();
  dup.root.children[0].id = "g-editor";
  assert.ok(rules(dup).includes("group-id-unique"));
  const badId = dflt();
  badId.root.children[0].id = "project";
  assert.ok(rules(badId).includes("group-id"));
});

test("validator: collapsed ids exist, are unique, and one group stays expanded; maximized is open", () => {
  const ghost = dflt();
  ghost.collapsed = ["g-nope"];
  assert.ok(rules(ghost).includes("collapsed-exists"));
  const all = dflt();
  all.collapsed = groupIds(all);
  assert.ok(rules(all).includes("collapsed-all"));
  const dup = dflt();
  dup.collapsed = ["g-bottom", "g-bottom"];
  assert.ok(rules(dup).includes("collapsed-unique"));
  const max = dflt();
  max.maximized = "ghost";
  assert.ok(rules(max).includes("maximized"));
});

test("validator: depth 8 is the limit", () => {
  const deep = (levels) => {
    let node = group("g-leaf", ["editor"]);
    let dir = "row";
    for (let level = 0; level < levels; level += 1) {
      node = split(dir, [0.5, 0.5], [node, group(`g-s${level}`, [`p${String.fromCharCode(97 + level)}`])]);
      dir = dir === "row" ? "column" : "row";
    }
    return node;
  };
  const make = (levels) => ({ ...dflt(), root: deep(levels), collapsed: [], closed: [], maximized: null });
  assert.deepEqual(rules(make(7)), [], "7 splits + a leaf is depth 8");
  assert.ok(rules(make(8)).includes("depth"), "8 splits + a leaf is depth 9");
});

test("validator: 64 KiB cap (document and stored text) and the shell block", () => {
  const big = dflt();
  big.closed = Array.from({ length: 9000 }, (_, index) => `x-${index}`);
  assert.ok(rules(big).includes("size-limit"));
  assert.equal(wb.parseLayout("x".repeat(70000)).ok, false);
  assert.equal(wb.parseLayout("{not json").ok, false);
  const shell = dflt();
  shell.shell.sideBar = "top";
  assert.ok(rules(shell).includes("shell"));
});

// ---- 3. operations ------------------------------------------------------------------------------------------

test("activatePanel: switches the tab, expands a collapsed group, reports the visibility change", () => {
  const { layout, change } = wb.activatePanel(dflt(), "scene");
  assert.equal(wb.findGroup(layout, "g-preview").active, "scene");
  assert.deepEqual(change.shown, ["scene"]);
  assert.deepEqual(change.hidden, ["canvas"]);
  const opened = wb.activatePanel(dflt(), "call-trace");
  assert.deepEqual(opened.layout.collapsed, [], "activating a panel of a collapsed group expands the group");
  assert.deepEqual(opened.change.shown, ["call-trace"]);
  assert.equal(wb.activatePanel(dflt(), "ghost").layout.root.type, "split");
});

test("collapseGroup: toggles, keeps size and tabs, refuses the last expanded group", () => {
  const out = wb.collapseGroup(dflt(), "g-bottom", false);
  assert.deepEqual(out.layout.collapsed, []);
  assert.deepEqual(out.change.shown, ["diagnostics"]);
  const back = wb.collapseGroup(out.layout, "g-bottom", true);
  assert.deepEqual(back.layout, dflt(), "collapse and expand round-trips");
  let layout = out.layout;
  for (const id of groupIds(layout).slice(0, -1)) {
    layout = wb.collapseGroup(layout, id, true).layout;
  }
  assert.equal(wb.collapseGroup(layout, groupIds(layout).at(-1), true).layout, layout, "the last expanded group stays");
});

test("closePanel: moves the id to closed, removes an emptied group, keeps the editor", () => {
  let { layout, change } = wb.closePanel(dflt(), "canvas");
  assert.deepEqual(layout.closed, ["canvas"]);
  assert.deepEqual(change.closed, ["canvas"]);
  assert.deepEqual(change.shown, ["scene"]);
  assert.equal(wb.findGroup(layout, "g-preview").active, "scene");
  layout = wb.closePanel(layout, "scene").layout;
  assert.equal(wb.findGroup(layout, "g-preview"), null, "an emptied group is removed");
  assertValid(layout, "after closing both preview panels");
  assert.equal(wb.closePanel(layout, "editor").layout, layout, "the editor cannot be closed");
});

test("openPanel: default location, activates an open one, honors `where`, expands the group", () => {
  const closed = wb.closePanel(wb.closePanel(dflt(), "canvas").layout, "scene").layout;
  const reopened = wb.openPanel(closed, "canvas");
  assert.deepEqual(reopened.layout.closed, ["scene"]);
  assert.ok(wb.groupOfPanel(reopened.layout, "canvas") !== null);
  assert.deepEqual(reopened.change.opened, ["canvas"]);
  assertValid(reopened.layout, "reopened");
  const where = wb.openPanel(closed, "canvas", { group: "g-editor" });
  assert.equal(wb.groupOfPanel(where.layout, "canvas").id, "g-editor");
  const hidden = wb.openPanel(dflt(), "call-trace");
  assert.deepEqual(hidden.layout.collapsed, [], "opening a panel of the collapsed panel area expands it");
  assert.equal(wb.openPanel(dflt(), "problems").layout.root.type, "split", "a reserved panel is not offered");
});

test("movePanel: into another group as a tab, at an index, and reordering inside a group", () => {
  const joined = wb.movePanel(dflt(), "canvas", { group: "g-editor" });
  assert.deepEqual(wb.findGroup(joined.layout, "g-editor").panels, ["editor", "canvas"]);
  assert.equal(wb.findGroup(joined.layout, "g-editor").active, "canvas");
  assert.equal(wb.findGroup(joined.layout, "g-preview").active, "scene");
  assertValid(joined.layout, "joined");
  const first = wb.movePanel(dflt(), "scene", { group: "g-preview", index: 0 });
  assert.deepEqual(wb.findGroup(first.layout, "g-preview").panels, ["scene", "canvas"]);
  const stay = wb.movePanel(dflt(), "canvas", { group: "g-preview", index: 2 });
  assert.deepEqual(wb.findGroup(stay.layout, "g-preview").panels, ["scene", "canvas"], "to the end of its own group");
});

test("movePanel: to an edge makes a new group; the source group disappears when emptied", () => {
  const out = wb.movePanel(dflt(), "canvas", { edge: "bottom", of: "g-editor" });
  assertValid(out.layout, "edge move");
  assert.equal(wb.groupOfPanel(out.layout, "canvas").id, "g-canvas");
  assert.equal(wb.groupOfPanel(out.layout, "scene").id, "g-preview");
  const root = wb.movePanel(dflt(), "scene", { edge: "right", of: "root" });
  assertValid(root.layout, "root edge move");
  assert.equal(root.layout.root.dir, "row");
  assert.equal(root.layout.root.children.at(-1).panels[0], "scene");
  const single = wb.movePanel(dflt(), "editor", { edge: "right", of: "g-editor" });
  assert.equal(single.layout.root.type, "split", "splitting a group around its only panel is a no-op");
  assert.deepEqual(single.change, { opened: [], closed: [], shown: [], hidden: [] });
  const emptied = wb.movePanel(dflt(), "project", { edge: "bottom", of: "g-editor" });
  assert.equal(wb.findGroup(emptied.layout, "g-project"), null);
  assertValid(emptied.layout, "project moved out of its group");
});

test("canonical flattening: a same-direction split never survives an operation", () => {
  const out = wb.movePanel(dflt(), "project", { edge: "right", of: "g-editor" });
  const walk = (node, parentDir) => {
    if (node.type === "group") return;
    assert.notEqual(node.dir, parentDir, "no same-direction nesting");
    assert.ok(Math.abs(node.sizes.reduce((a, b) => a + b, 0) - 1) < 1e-9);
    node.children.forEach((child) => walk(child, node.dir));
  };
  walk(out.layout.root, null);
  const collapsedAway = wb.movePanel(dflt(), "canvas", { group: "g-editor" });
  const closed = wb.closePanel(collapsedAway.layout, "scene").layout;
  assertValid(closed, "after a join and a close");
  assert.equal(wb.normalize(closed.root).type, "split");
});

test("splitGroup, resizeSplit, maximize, resetLayout, applyPreset", () => {
  const sp = wb.splitGroup(dflt(), "g-editor", "column", "scene");
  assert.equal(wb.groupOfPanel(sp.layout, "scene").id, "g-scene");
  assertValid(sp.layout, "split");
  const rs = wb.resizeSplit(dflt(), [], [0.3, 0.7]);
  assert.deepEqual(rs.layout.root.sizes, [0.3, 0.7]);
  assert.equal(wb.resizeSplit(dflt(), [], [0.3]).layout.root.sizes[0], 0.18, "a wrong length is ignored");
  assert.equal(wb.resizeSplit(dflt(), [9], [0.5, 0.5]).layout.root.sizes[0], 0.18, "a missing path is ignored");
  assert.deepEqual(wb.resizeSplit(dflt(), [1], [0.5, 0.5]).layout.root.children[1].sizes, [0.5, 0.5]);
  const mx = wb.maximize(dflt(), "scene");
  assert.equal(mx.layout.maximized, "scene");
  assert.deepEqual(mx.change.shown, ["scene"]);
  assert.deepEqual(mx.change.hidden, ["canvas", "editor", "project"]);
  assert.equal(wb.maximize(mx.layout, null).layout.maximized, null);
  const moved = wb.movePanel(wb.closePanel(dflt(), "canvas").layout, "scene", { edge: "bottom", of: "g-editor" });
  const reset = wb.resetLayout(moved.layout);
  assert.deepEqual(reset.layout, dflt());
  assert.deepEqual(reset.change.opened, ["canvas"]);
  const preset = wb.applyPreset(dflt(), "editor-only");
  assert.deepEqual(preset.layout.closed, ["canvas", "scene"]);
  assert.deepEqual(preset.change.closed, ["canvas", "scene"]);
  assert.equal(wb.applyPreset(dflt(), "nope").layout.root.type, "split");
});

test("applyOperation dispatches every operation kind", () => {
  const ops = [
    { kind: "open", panelId: "scene" },
    { kind: "close", panelId: "canvas" },
    { kind: "move", panelId: "canvas", target: { group: "g-editor" } },
    { kind: "activate", panelId: "scene" },
    { kind: "split", groupId: "g-editor", dir: "row", panelId: "scene" },
    { kind: "resize", path: [], sizes: [0.4, 0.6] },
    { kind: "maximize", panelId: "scene" },
    { kind: "reset" },
    { kind: "collapse", groupId: "g-bottom", collapsed: false },
    { kind: "preset", presetId: "side-by-side" },
  ];
  for (const op of ops) {
    const out = wb.applyOperation(dflt(), op);
    assertValid(out.layout, `op ${op.kind}`);
  }
});

// ---- 4. migration, reconcile, newer documents ----------------------------------------------------------------

test("fixtures: every tests/fixtures/studio-layout/v<n>.json is valid and reconciles to a valid layout", () => {
  const dir = resolve(REPO, "tests", "fixtures", "studio-layout");
  const files = readdirSync(dir).filter((name) => /^v\d+\.json$/.test(name));
  assert.ok(files.includes("v1.json"));
  for (const name of files) {
    const migrated = wb.migrateLayout(fixture(name));
    assert.ok(migrated.ok, `${name} migrates to the current version`);
    assert.equal(migrated.doc.version, wb.LAYOUT_VERSION);
    assertValid(wb.reconcile(migrated.doc), name);
  }
});

test("migrateLayout: a chain of steps in order, a missing step, a newer and an invalid version", () => {
  const table = {
    1: (doc) => ({ ...doc, toolbarHidden: true, trail: "1" }),
    2: (doc) => ({ ...doc, trail: `${doc.trail}2` }),
  };
  const ok = wb.migrateLayout({ ...dflt(), version: 1 }, table, 3);
  assert.ok(ok.ok && ok.doc.version === 3 && ok.doc.trail === "12");
  assert.deepEqual(wb.migrateLayout({ ...dflt(), version: 1 }, { 1: (d) => d }, 3), { ok: false, reason: "missing-step", version: 1 });
  assert.equal(wb.migrateLayout({ ...dflt(), version: 9 }).reason, "newer");
  assert.equal(wb.migrateLayout({ version: "1" }).reason, "invalid");
  assert.equal(wb.migrateLayout([]).reason, "invalid");
});

test("reconcile: a new panel appears, a closed one stays closed, the editor is restored, unknown ids drop", () => {
  const doc = fixture("v1.json");
  const out = wb.reconcile(doc);
  assert.ok(wb.groupOfPanel(out, "platform-events") !== null, "platform-events is new for this document");
  assert.equal(wb.groupOfPanel(out, "call-trace"), null, "a closed panel stays closed");
  assert.deepEqual(out.closed, ["call-trace"]);
  const noEditor = clone(doc);
  noEditor.root.children[1].children[0].panels = ["editor", "ghost"];
  noEditor.root.children[1].children[0].panels = ["ghost"];
  noEditor.root.children[1].children[0].active = "ghost";
  const restored = wb.reconcile(noEditor);
  assert.ok(wb.groupOfPanel(restored, "editor") !== null, "the editor is restored");
  assert.equal(JSON.stringify(restored).includes("ghost"), false, "unknown panel ids are dropped");
  assertValid(restored, "restored");
  const reserved = clone(doc);
  reserved.root.children[0].panels.push("problems");
  assert.equal(wb.groupOfPanel(wb.reconcile(reserved), "problems"), null, "a reserved panel is dropped");
});

// ---- 5. phone collapse -----------------------------------------------------------------------------------------

test("phone collapse: Editor, Canvas, Scene, Diagnostics first; the rest in reading order; layout untouched", () => {
  const layout = dflt();
  const before = clone(layout);
  const phone = wb.collapse(layout);
  assert.deepEqual(phone.panels, ["editor", "canvas", "scene", "diagnostics", "project", "mock-console", "call-trace", "platform-events"]);
  assert.equal(phone.active, "editor");
  assert.equal(wb.collapse(layout, "scene").active, "scene");
  assert.equal(wb.collapse(layout, "ghost").active, "editor");
  assert.deepEqual(layout, before);
  const closed = wb.closePanel(layout, "canvas").layout;
  assert.deepEqual(wb.collapse(closed).panels.slice(0, 3), ["editor", "scene", "diagnostics"]);
  assert.equal(wb.isPhoneWidth(719), true);
  assert.equal(wb.isPhoneWidth(720), false);
});

// ---- 6. persistence -----------------------------------------------------------------------------------------------

function memoryStorage(initial = {}) {
  const map = new Map(Object.entries(initial));
  return {
    map,
    getItem: (key) => (map.has(key) ? map.get(key) : null),
    setItem: (key, value) => void map.set(key, value),
  };
}

test("persistence: round trip through the saver (debounced 300 ms) and the loader", () => {
  const storage = memoryStorage();
  const timers = { queue: [], setTimeout(fn, ms) { this.queue.push({ fn, ms }); return this.queue.length; }, clearTimeout(handle) { this.queue[handle - 1] = null; } };
  const saver = wb.createLayoutSaver(storage, timers);
  const first = wb.movePanel(dflt(), "canvas", { edge: "bottom", of: "g-editor" }).layout;
  const second = wb.closePanel(first, "scene").layout;
  saver.schedule(first);
  saver.schedule(second);
  assert.equal(storage.map.size, 0, "nothing is written before the debounce fires");
  const live = timers.queue.filter(Boolean);
  assert.equal(live.length, 1, "a second schedule replaces the first timer");
  assert.equal(live[0].ms, 300);
  live[0].fn();
  const loaded = wb.loadLayout(storage);
  assert.equal(loaded.source, "stored");
  assert.deepEqual(loaded.layout, second);
  const zen = clone(second);
  zen.shell.zen = true;
  assert.equal(JSON.parse(wb.serializeLayout(zen)).shell.zen, false, "zen is never stored");
});

test("persistence: a newer version keeps a backup, uses the default and says so", () => {
  const newer = JSON.stringify({ ...dflt(), version: 99, futureField: true });
  const storage = memoryStorage({ [wb.LAYOUT_STORAGE_KEY]: newer });
  const loaded = wb.loadLayout(storage);
  assert.equal(loaded.source, "default");
  assert.match(loaded.notice, /newer studio/);
  assert.equal(storage.map.get(wb.LAYOUT_BACKUP_KEY), newer);
  assert.equal(storage.map.get(wb.LAYOUT_STORAGE_KEY), newer, "the newer document is not rewritten");
});

test("persistence: corrupt, invalid and oversized documents give the default (never an error)", () => {
  const invalid = JSON.stringify({ ...dflt(), root: { type: "split", dir: "row", sizes: [0.9, 0.9], children: [] } });
  for (const bad of ["{oops", invalid, "[]", "x".repeat(70000)]) {
    const storage = memoryStorage({ [wb.LAYOUT_STORAGE_KEY]: bad });
    const loaded = wb.loadLayout(storage);
    assert.equal(loaded.source, "default");
    assert.ok(loaded.notice !== null);
    assertValid(loaded.layout, "fallback");
  }
});

test("persistence: throwing, missing and quota-exceeded storage render the default silently", () => {
  assert.equal(wb.loadLayout(null).source, "default");
  const throwing = { getItem() { throw new Error("blocked"); }, setItem() { throw new Error("blocked"); } };
  const loaded = wb.loadLayout(throwing);
  assert.equal(loaded.source, "default");
  assert.equal(loaded.notice, null);
  const timers = { setTimeout: (fn) => { fn(); return 1; }, clearTimeout() {} };
  assert.doesNotThrow(() => wb.createLayoutSaver(throwing, timers).schedule(dflt()));
  assert.doesNotThrow(() => wb.createLayoutSaver(null, timers).flush());
  const was = globalThis.localStorage;
  Object.defineProperty(globalThis, "localStorage", { configurable: true, get() { throw new Error("denied"); } });
  try {
    assert.equal(wb.browserStorage(), null);
  } finally {
    if (was === undefined) delete globalThis.localStorage;
  }
});

test("persistence: a layout over 64 KiB is not written", () => {
  const storage = memoryStorage();
  const timers = { setTimeout: (fn) => { fn(); return 1; }, clearTimeout() {} };
  const big = dflt();
  big.closed = Array.from({ length: 9000 }, (_, index) => `x-${index}`);
  wb.createLayoutSaver(storage, timers).schedule(big);
  assert.equal(storage.map.size, 0);
});

// ---- 7. the panel host contract -----------------------------------------------------------------------------

test("panel runtime: visibility transitions are reported once, hidden panels get false, close fires onClose", () => {
  const log = [];
  const runtime = wb.createPanelRuntime();
  for (const id of ["canvas", "scene", "diagnostics", "editor", "project"]) {
    runtime.register(id, { mount() {}, onVisibility: (v) => log.push(`${id}:${v}`), onClose: () => log.push(`${id}:close`) });
  }
  const inputs = { documentVisible: true, phoneActive: null };
  let layout = dflt();
  runtime.update(layout, inputs);
  assert.deepEqual([...log].sort(), ["canvas:true", "editor:true", "project:true"], "the collapsed panel area is never visible, so Diagnostics never is");
  log.length = 0;
  runtime.update(layout, inputs);
  assert.deepEqual(log, [], "an unchanged layout reports nothing");
  layout = wb.activatePanel(layout, "scene").layout;
  runtime.update(layout, inputs);
  assert.deepEqual(log, ["canvas:false", "scene:true"]);
  log.length = 0;
  runtime.update(layout, { documentVisible: false, phoneActive: null });
  assert.deepEqual(log.sort(), ["editor:false", "project:false", "scene:false"], "a hidden document hides every panel");
  log.length = 0;
  runtime.update(layout, inputs);
  log.length = 0;
  layout = wb.closePanel(layout, "scene").layout;
  runtime.update(layout, inputs);
  assert.ok(log.includes("scene:close") && log.includes("scene:false"));
  assert.equal(runtime.isOpen("scene"), false);
  log.length = 0;
  runtime.update(layout, { documentVisible: true, phoneActive: "project" });
  assert.deepEqual(log.sort(), ["canvas:false", "editor:false"], "phone shows one panel");
  assert.equal(runtime.isVisible("project"), true);
  assert.equal(runtime.isVisible("editor"), false);
  assert.equal(runtime.isVisible("canvas"), false);
});

// ---- 8. layout never travels with a project or a share link -------------------------------------------------

test("the share codec and the project package carry no layout", async () => {
  for (const file of ["src/lib/share.js", "src/lib/preview-state.js", "project-package.js", "project-io.js"]) {
    const text = readFileSync(resolve(WEB, file), "utf8");
    assert.equal(/tessera\.studio\.layout|workbench/.test(text), false, `${file} must not mention the layout`);
  }
  const share = await import(new URL("../../designer/web/src/lib/share.js", import.meta.url).href);
  const state = {
    source: "export const A = () => <div/>;", file: "ui/A.tsx", width: 320, height: 240, dpr: 1, theme: "system",
    foldPosture: "", skin: "", profile: "",
  };
  const fragment = await share.encodeShare({ ...state, layout: dflt() });
  const decoded = await share.decodeShare(fragment);
  assert.equal(Object.keys(decoded).some((key) => /layout/i.test(key)), false, "a layout field in the state is not encoded");
  assert.equal(fragment.includes("studio"), false);
});

// ---- 9. keyboard moves, the store and the plain command functions ----------------------------------------------

test("keyboard moves: Down joins the panel area (and opens it), Right and Left make a column at the edge", () => {
  const layout = dflt();
  assert.deepEqual(wb.directionTarget(layout, "canvas", "bottom"), { group: "g-bottom" });
  assert.deepEqual(wb.directionTarget(layout, "diagnostics", "right"), { edge: "right", of: "root" });
  assert.deepEqual(wb.directionTarget(layout, "scene", "left"), { edge: "left", of: "root" });
  assert.equal(wb.directionTarget(layout, "diagnostics", "bottom"), null, "already in the panel area");
  assert.equal(wb.directionTarget(layout, "project", "left"), null, "a lone panel already at that edge");
  assert.equal(wb.directionTarget(layout, "ghost", "right"), null);
  const down = wb.movePanel(layout, "canvas", wb.directionTarget(layout, "canvas", "bottom"));
  assert.deepEqual(down.layout.collapsed, [], "moving into the collapsed panel area opens it");
  assert.deepEqual(wb.findGroup(down.layout, "g-bottom").panels.slice(-1), ["canvas"]);
  const right = wb.movePanel(wb.collapseGroup(layout, "g-bottom", false).layout, "diagnostics", { edge: "right", of: "root" });
  assertValid(right.layout, "diagnostics moved right");
  assert.equal(right.layout.root.children.at(-1).panels[0], "diagnostics");
});

test("keyboard moves: the group submenu names groups by their active panel; the live message", () => {
  const choices = wb.groupChoices(dflt(), "canvas", wb.PANELS);
  assert.deepEqual(choices.map((choice) => choice.label), ["Project", "Editor", "Diagnostics"]);
  assert.equal(wb.moveMessage("Canvas", "bottom"), "Canvas moved to the bottom");
});

test("store and commands: toggles, reveal, preset, reset, maximize, change notifications", () => {
  const store = wb.createLayoutStore(dflt());
  const commands = wb.createCommands(store);
  const seen = [];
  store.subscribe((result) => seen.push(result.change));
  assert.equal(wb.panelAreaOpen(store.get()), false, "the panel area starts collapsed");
  commands.togglePanelArea();
  assert.equal(wb.panelAreaOpen(store.get()), true);
  assert.deepEqual(seen.at(-1).shown, ["diagnostics"]);
  commands.togglePanelArea();
  assert.equal(wb.panelAreaOpen(store.get()), false);
  commands.revealPanelArea();
  commands.revealPanelArea();
  assert.equal(seen.length, 3, "revealing an open panel area changes nothing and notifies no one");
  commands.togglePanel("scene");
  assert.equal(wb.groupOfPanel(store.get(), "scene").active, "scene", "a hidden open panel is revealed, not closed");
  commands.togglePanel("scene");
  assert.deepEqual(store.get().closed, ["scene"], "a visible panel is closed");
  commands.togglePanel("scene");
  assert.deepEqual(store.get().closed, []);
  assert.equal(commands.isDefault(), false);
  commands.maximizePanel();
  assert.ok(store.get().maximized !== null);
  commands.maximizePanel();
  assert.equal(store.get().maximized, null);
  commands.layoutPreset("editor-only");
  assert.deepEqual(store.get().closed, ["canvas", "scene"]);
  commands.resetLayout();
  assert.deepEqual(store.get(), dflt());
  assert.equal(commands.isDefault(), true);
  commands.movePanelDirection("canvas", "bottom");
  assert.equal(wb.groupOfPanel(store.get(), "canvas").id, "g-bottom");
});

test("visible resize: a sash on the folded tree resizes the real tree and collapsed groups keep their share", () => {
  const layout = dflt(); // g-bottom collapsed, so the screen shows row[project, editor, preview]
  assert.equal(wb.visibleLayout(layout).root.children.length, 3);
  const first = wb.applyOperation(layout, { kind: "resize", path: [], sizes: [0.2, 0.4, 0.4], visible: true }).layout;
  assert.deepEqual(first.root.sizes, [0.2, 0.8], "the first sash moves the real root boundary");
  assert.deepEqual(first.root.children[1].sizes, [0.72, 0.28], "the collapsed panel area keeps its share");
  assert.deepEqual(first.root.children[1].children[0].sizes, [0.5, 0.5]);
  const second = wb.applyOperation(layout, { kind: "resize", path: [], sizes: [0.18, 0.5, 0.32], visible: true }).layout;
  assert.deepEqual(second.root.sizes, [0.18, 0.82]);
  assert.deepEqual(second.root.children[1].children[0].sizes, [0.609756, 0.390244], "the second sash moves the inner row's boundary");
  assertValid(second, "after a visible resize");
  const bad = wb.applyOperation(layout, { kind: "resize", path: [], sizes: [0.5, 0.5], visible: true });
  assert.equal(bad.layout, layout, "a wrong number of sizes is ignored");
  const open = wb.collapseGroup(layout, "g-bottom", false).layout;
  assert.deepEqual(wb.applyOperation(open, { kind: "resize", path: [1], sizes: [0.6, 0.4], visible: true }).layout.root.children[1].sizes, [0.6, 0.4], "nothing collapsed: visible and real paths agree");
});

process.stdout.write(`workbench_layout: ${passed} passed, ${failures} failed\n`);
process.exit(failures === 0 ? 0 : 1);
