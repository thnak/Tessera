// SPDX-License-Identifier: MIT
//
// The Studio Resources controls' acceptance check (issue #657; `designer-protocol-v1.md` section 5.2,
// `pending-ui-v1.md` section 10.4).
//
//   node scripts/ci/preview_resource_controls.mjs [--scene-module build/.../tessera_scene_preview.js]
//                                                 [--canvas-module build/.../tessera_canvas.js]
//
// Three layers, kept apart because they need different things:
//
//  * **The page's vocabulary and call** (always run): the request builder, the report parser and the
//    sentences the panel shows, and `sendResourceControl` against a stand-in module that records what the
//    export receives -- which pins the ABI argument order (strings first, then numbers: action, resource,
//    text, number, epoch). These are the modules the page ships (`dist-ssr/resource-controls.js` and
//    `canvas-resource.js`), so `npm run build:chrome --prefix designer/web` is a prerequisite; a missing
//    build is exit code 2.
//
//  * **The engine, under Node** (with `--scene-module`): Node has no WebGPU device, so a canvas session
//    cannot start; the scene module's `tessera_input_json` runs the same `PreviewSession` and accepts the
//    same controls as `resource <action> <name> [value] [@epoch]` script steps. A component with a
//    suspending resource is staged and driven through the whole cycle -- fallback on open, a resolve that
//    restores the content, a stale epoch that is dropped, a refresh that keeps the content, a failure that
//    keeps the data -- and every reply goes through the page's own `parseReport`.
//
//  * **The canvas export** (with `--canvas-module`): it exists, takes the page's argument order, and
//    answers a well-formed report when no session can start.
//
// Deterministic: no clock, no network. Exit codes: 0 all passed, 1 a check failed, 2 setup error.

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const DIST_SSR = resolve(HERE, "..", "..", "designer", "web", "dist-ssr");

let failures = 0;
async function check(name, body) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

function option(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : null;
}

let controls;
let canvasResource;
try {
  controls = await import(pathToFileURL(resolve(DIST_SSR, "resource-controls.js")).href);
  canvasResource = await import(pathToFileURL(resolve(DIST_SSR, "canvas-resource.js")).href);
} catch {
  process.stderr.write(
    "SETUP ERROR: dist-ssr/resource-controls.js is missing. Build the page first:\n" +
      "  npm ci --prefix designer/web && npm run build:chrome --prefix designer/web\n",
  );
  process.exit(2);
}
const { STATUS_REQUEST, describePending, describeResource, draftError, parseReport, requestFor, unavailableReport } =
  controls;
const { sendResourceControl, supportsResourceControl } = canvasResource;

const REPORT = (extra = {}) =>
  JSON.stringify({
    pendingVersion: 1,
    ok: true,
    accepted: true,
    changed: true,
    isPending: false,
    resources: [{ id: "res::feed", status: "loading", epoch: 1, error: "" }],
    boundaries: [{ id: "node::2", fallback: true, held: false }],
    error: "",
    ...extra,
  });

/** A stand-in module: strings live in a map keyed by pointer, and the export records what it receives. */
function fakeModule(reply) {
  const heap = new Map();
  let next = 16;
  const freed = [];
  const module = {
    calls: [],
    freed,
    _malloc(size) {
      const pointer = next;
      next += size + 8;
      heap.set(pointer, "");
      return pointer;
    },
    _free(pointer) {
      freed.push(pointer);
    },
    _tessera_free(pointer) {
      freed.push(pointer);
    },
    lengthBytesUTF8: (text) => Buffer.byteLength(text),
    stringToUTF8(text, pointer) {
      heap.set(pointer, text);
    },
    UTF8ToString: (pointer) => heap.get(pointer),
  };
  module._tessera_canvas_resource = (action, resource, text, number, epoch) => {
    module.calls.push({ action: heap.get(action), resource: heap.get(resource), text: heap.get(text), number, epoch });
    const out = module._malloc(8);
    heap.set(out, reply);
    return out;
  };
  return module;
}

await check("requestFor builds the request each button sends", () => {
  assert.deepEqual(requestFor("fetch", "res::feed", { value: "ignored", epoch: "3" }), {
    action: "fetch", resource: "res::feed", number: 0, text: "", epoch: 0,
  });
  assert.deepEqual(requestFor("resolve", "res::feed", { value: "hello", epoch: "" }), {
    action: "resolve", resource: "res::feed", number: 0, text: "hello", epoch: 0,
  });
  assert.deepEqual(requestFor("resolve", "res::count", { value: "42", epoch: "7" }), {
    action: "resolve", resource: "res::count", number: 42, text: "42", epoch: 7,
  });
  assert.deepEqual(requestFor("fail", "res::feed", { value: "boom", epoch: "" }), {
    action: "fail", resource: "res::feed", number: 0, text: "boom", epoch: 0,
  });
});

await check("an epoch draft is a whole number from 1, or empty", () => {
  assert.equal(draftError({ value: "", epoch: "" }), null);
  assert.equal(draftError({ value: "", epoch: " 12 " }), null);
  for (const bad of ["0", "-1", "1.5", "abc", "12345678901"]) {
    assert.notEqual(draftError({ value: "", epoch: bad }), null, bad);
  }
  assert.equal(requestFor("resolve", "r", { value: "x", epoch: "oops" }).epoch, 0, "a bad draft is never sent as an epoch");
});

await check("the status request reads and applies nothing", () => {
  assert.equal(STATUS_REQUEST.action, "status");
  assert.equal(Object.isFrozen(STATUS_REQUEST), true);
});

await check("parseReport reads the engine's report and never throws", () => {
  const report = parseReport(REPORT());
  assert.equal(report.ok, true);
  assert.equal(report.resources[0].id, "res::feed");
  assert.equal(report.boundaries[0].fallback, true);
  assert.equal(parseReport("not json").ok, false);
  assert.equal(parseReport(JSON.stringify({ pendingVersion: 2 })).ok, false);
  assert.equal(parseReport("null").ok, false);
  assert.equal(unavailableReport("x").error, "x");
});

await check("the panel's sentences name the fallback, the held content and isPending", () => {
  const opening = parseReport(REPORT());
  assert.equal(describePending(opening), "A boundary shows its fallback. isPending is false.");
  assert.equal(describeResource(opening.resources[0]), "loading (epoch 1)");
  const held = parseReport(
    REPORT({
      isPending: true,
      resources: [{ id: "res::feed", status: "refreshing", epoch: 3, error: "" }],
      boundaries: [{ id: "node::2", fallback: false, held: true }],
    }),
  );
  assert.equal(describePending(held), "A boundary holds its content. isPending is true.");
  const content = parseReport(REPORT({ boundaries: [{ id: "node::2", fallback: false, held: false }] }));
  assert.equal(describePending(content), "The content is showing. isPending is false.");
  const failed = parseReport(REPORT({ resources: [{ id: "res::feed", status: "error", epoch: 2, error: "boom" }] }));
  assert.equal(describeResource(failed.resources[0]), "error (epoch 2): boom");
  assert.equal(describePending(parseReport(REPORT({ resources: [], boundaries: [] }))), "This component declares no resource.");
});

await check("sendResourceControl passes strings first, then numbers, and frees what it allocated", () => {
  const module = fakeModule(REPORT());
  assert.equal(supportsResourceControl(module), true);
  const report = sendResourceControl(module, { action: "resolve", resource: "res::feed", number: 4, text: "hello", epoch: 2 });
  assert.equal(report.ok, true);
  assert.deepEqual(module.calls, [{ action: "resolve", resource: "res::feed", text: "hello", number: 4, epoch: 2 }]);
  assert.equal(module.freed.length, 4, "three argument strings and the reply are released");
});

await check("a module without the export, or one that returns nothing, is an unavailable report", () => {
  const old = fakeModule(REPORT());
  delete old._tessera_canvas_resource;
  assert.equal(supportsResourceControl(old), false);
  const missing = sendResourceControl(old, STATUS_REQUEST);
  assert.equal(missing.ok, false);
  assert.match(missing.error, /no resource controls/);
  const broken = fakeModule(REPORT());
  broken._tessera_canvas_resource = () => 0;
  const none = sendResourceControl(broken, STATUS_REQUEST);
  assert.equal(none.ok, false);
  assert.match(none.error, /returned no document/);
});

const SOURCE = [
  'import { useState, useService, useResource, useTransition, useDeferredValue, Suspense } from "tessera";',
  "export function Feed() {",
  "  const nav = useService<Navigator>();",
  '  const [mode, setMode] = useState<"fast" | "slow">("fast");',
  "  const [isPending, startTransition] = useTransition();",
  "  const settled = useDeferredValue(mode);",
  "  const feed = useResource(nav.loadFeed, { mode }, { suspend: true });",
  "  return (",
  "    <View>",
  "      <Suspense fallback={<Text>Loading</Text>}>",
  "        <View><Text>{feed.data}</Text></View>",
  "      </Suspense>",
  '      <Button onClick={() => startTransition(() => setMode("slow"))}>',
  "        <Text>Slow</Text>",
  "      </Button>",
  "      {isPending && <Text>Updating</Text>}",
  "      <Text>{settled}</Text>",
  "    </View>",
  "  );",
  "}",
  "",
].join("\n");

const MANIFEST = JSON.stringify({
  version: 1,
  services: [
    {
      id: "demo.nav",
      name: "Navigator",
      cpp: { type: "demo::Navigator", header: "demo/services.hpp" },
      methods: [{ name: "loadFeed", params: [{ name: "mode", type: "string" }], returns: "string" }],
    },
  ],
});

function call(module, name, strings, numbers = []) {
  const pointers = strings.map((text) => {
    const size = module.lengthBytesUTF8(text) + 1;
    const pointer = module._malloc(size);
    module.stringToUTF8(text, pointer, size);
    return pointer;
  });
  try {
    const result = module[name](...pointers, ...numbers);
    if (!result) {
      return null;
    }
    const text = module.UTF8ToString(result);
    module._tessera_free(result);
    return text;
  } finally {
    pointers.forEach((pointer) => module._free(pointer));
  }
}

const sceneModule = option("--scene-module");
if (sceneModule !== null) {
  const module = await (await import(pathToFileURL(resolve(sceneModule)).href)).default();
  const stage = () => {
    call(module, "_tessera_project_begin", []);
    call(module, "_tessera_project_add_source", ["ui/Feed.tsx", SOURCE]);
    call(module, "_tessera_project_set_services", [MANIFEST]);
    call(module, "_tessera_project_set_entry", ["ui/Feed.tsx"]);
  };
  const run = (script) => JSON.parse(call(module, "_tessera_input_json", ["", script]));
  const reports = (document) => document.steps.map((step) => parseReport(JSON.stringify(step.report)));

  await check("the scene module runs a resource through its whole cycle (fallback, content, stale, refresh, failure)", () => {
    stage();
    const document = run(
      [
        "resource status",
        "resource resolve feed hello world",
        "resource resolve res::feed late @9",
        "resource fetch feed",
        "resource resolve feed fresh",
        "resource fetch feed",
        "resource fail feed boom",
      ].join("\n"),
    );
    assert.equal(document.error, undefined, document.error);
    const [opening, resolved, stale, refreshing, refreshed, again, failed] = reports(document);
    assert.equal(opening.ok && !opening.changed, true, "status is a read");
    assert.equal(opening.resources[0].status, "loading");
    assert.equal(describePending(opening), "A boundary shows its fallback. isPending is false.");
    assert.equal(resolved.accepted && resolved.changed, true);
    assert.equal(resolved.resources[0].status, "success");
    assert.equal(describePending(resolved), "The content is showing. isPending is false.");
    assert.equal(stale.ok, true);
    assert.equal(stale.accepted, false, "an epoch that is not the current one is dropped");
    assert.equal(stale.changed, false, "a dropped completion owes no frame");
    assert.equal(stale.resources[0].status, "success", "and changes nothing");
    assert.equal(refreshing.resources[0].status, "refreshing");
    assert.equal(refreshing.boundaries[0].fallback, false, "a refresh keeps the content");
    assert.equal(refreshed.resources[0].status, "success");
    assert.equal(again.resources[0].status, "refreshing");
    assert.equal(failed.resources[0].status, "error");
    assert.equal(failed.resources[0].error, "boom");
    assert.equal(failed.boundaries[0].fallback, false, "a failed refresh keeps the old data");
  });

  await check("an unknown resource and an unknown action are reported, never applied", () => {
    stage();
    const unknown = run("resource fetch nope");
    const report = parseReport(JSON.stringify(unknown.steps[0].report));
    assert.equal(report.ok, false);
    assert.match(report.error, /no resource/);
    const bad = run("resource explode feed");
    assert.match(bad.error, /needs fetch, resolve, fail or status/);
  });
}

// The page hands the project's `ui/services.json` to the engine (#976): without it a `useResource` component
// has no service to bind and the Resources panel never appears. Needs Node with type stripping (v22.18+).
await check("the project snapshot carries ui/services.json as its buffer holds it", async () => {
  const { projectSnapshot } = await import(
    pathToFileURL(resolve(HERE, "..", "..", "designer", "web", "src", "lib", "project-snapshot.ts")).href
  );
  const file = (path, kind) => ({ path, name: path, kind, text: "", editable: true });
  const files = [file("tessera.json", "config"), file("ui/App.tsx", "source"), file("ui/services.json", "services")];
  const texts = { "tessera.json": '{"app":{"entry":"ui/App.tsx"}}', "ui/App.tsx": "x", "ui/services.json": MANIFEST };
  const snapshot = projectSnapshot(files, (path) => texts[path] ?? "", "ui/App.tsx");
  assert.equal(snapshot.servicesManifest, MANIFEST);
  const without = projectSnapshot(files.slice(0, 2), (path) => texts[path] ?? "", "ui/App.tsx");
  assert.equal("servicesManifest" in without, false);
  const blank = projectSnapshot(files, (path) => (path === "ui/services.json" ? "  \n" : (texts[path] ?? "")), "ui/App.tsx");
  assert.equal("servicesManifest" in blank, false);
});

const canvasModule = option("--canvas-module");
if (canvasModule !== null) {
  const module = await (await import(pathToFileURL(resolve(canvasModule)).href)).default();
  await check("the canvas module exports resource control and answers when no session can start", () => {
    assert.equal(supportsResourceControl(module), true, "the module does not export _tessera_canvas_resource");
    const report = sendResourceControl(module, { action: "fetch", resource: "res::feed", number: 0, text: "", epoch: 0 });
    assert.equal(report.ok, false);
    assert.match(report.error, /no canvas session/);
    const status = sendResourceControl(module, STATUS_REQUEST);
    assert.equal(status.ok, false, "a status read before any apply is an error, not a crash");
  });
}

process.stdout.write(failures === 0 ? "\nresource control checks: all passed\n" : `\nresource control checks: ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
