// SPDX-License-Identifier: MIT
//
// Smoke test for the studio page's shared chrome (issue #169, P5; the page it mounts on is the merged
// `index.html`, issue #392; the chrome is React since issue #422 and ADR-054).
//
//   node scripts/ci/preview_studio_smoke.mjs
//
// **What this is, and what it is not.** It is a *plain-Node* test of the chrome's behaviour, run against the
// built artifacts, with a stub browser. It is NOT a browser test, and it does not prove the page paints: the
// browser half -- that the bundle loads, that the CSS applies, that `location.hash` round-trips in a real
// navigation, that the three views switch -- is what the Pages lane and a human opening
// https://thnak.github.io/Tessera/ verify (`preview_browser_check.mjs` is the developer-run browser half).
// Saying this plainly is the point: a smoke test that claimed to be a browser test would be the kind of green
// that hides a blank page.
//
// **How a React chrome is tested without a browser.** Two layers, because the conversion moved the behaviour
// out of one hand-written module and into components plus DOM-free library modules:
//
//   1. **The library layer, imported from source.** `designer/web/src/lib/preferences.ts` (the toolbar's
//      persistence) and `designer/web/src/lib/sample-folder.ts` (a picked
//      folder's sample entries and companion files) have no DOM beyond `localStorage`/`location`/`navigator`, both of which
//      this file stubs. They are compiled to `dist-ssr/` by the same build that renders the chrome, so this
//      runs the shipped code and not a copy.
//   2. **The rendered chrome, from `dist-ssr/chrome.js`.** The components are rendered with
//      `renderToStaticMarkup`, which is how the ids, roles and initial state are asserted without a browser
//      (ADR-054 decision 6; `preview_page_checks.mjs` owns the exhaustive id list).
//
// What it covers, and why each case is worth having:
//   * the app mounts at all -- the build produced a `dist/` and a `dist-ssr/`, or this says so;
//   * the toolbar's three selects carry the documented option sets, in the rendered markup;
//   * the controls persist to `localStorage` under one key, and the source never does;
//   * the initial state comes from storage, then the page's default (a leftover share fragment is ignored);
//   * a sample opens from the folder the visitor picked: its entries, the library files its imports need
//     (copied from packages/tessera-ui when the folder has them), and a named diagnostic when it does not.

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, "..", "..");
const WEB = resolve(REPO_ROOT, "designer", "web");
const DIST = resolve(WEB, "dist");
const DIST_SSR = resolve(WEB, "dist-ssr");

let failures = 0;
// `async` + `await body()` is not decoration: the code under test is asynchronous (it awaits `fetch` and the
// codec), so a synchronous `test()` would print "ok" before the body had run and report a green suite that
// asserted nothing.
async function test(name, body) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

// --- the built artifacts, or a setup error that names the command -----------------------------------

for (const [path, what] of [
  [resolve(DIST, "index.html"), "the built site"],
  [resolve(DIST_SSR, "ssr-chrome.js"), "the built chrome"],
]) {
  try {
    readFileSync(path);
  } catch {
    process.stderr.write(
      `SETUP ERROR: ${what} is missing (${path}). Build the page first:\n` +
        "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
    );
    process.exit(2);
  }
}

// --- a stub browser ---------------------------------------------------------------------------------
//
// The library modules touch exactly four platform globals, and each is stubbed rather than mocked over: a
// `Storage` that records, a `location` whose hash the test controls, a `history` that records its rewrites,
// and a `navigator` with a clipboard. Nothing here re-implements the code under test.

const STORAGE_KEY = "tessera.preview.p5";

function installBrowser({ hash = "", clipboard = true } = {}) {
  const storage = new Map();
  const historyCalls = [];
  globalThis.localStorage = {
    getItem: (key) => (storage.has(key) ? storage.get(key) : null),
    setItem: (key, value) => storage.set(key, String(value)),
    removeItem: (key) => storage.delete(key),
  };
  globalThis.history = { replaceState: (state, title, url) => historyCalls.push(url) };
  globalThis.location = {
    origin: "https://example.test",
    pathname: "/Tessera/",
    search: "",
    hash,
  };
  // Node exposes `navigator` as a getter-only global, so a plain assignment throws in strict module code.
  // `defineProperty` is how a test replaces a platform global.
  Object.defineProperty(globalThis, "navigator", {
    configurable: true,
    value: clipboard
      ? { clipboard: { writeText: async (text) => historyCalls.push(text) } }
      : {},
  });
  return { storage, historyCalls };
}

// `preferences.ts` and `sample-folder.ts` are TypeScript; the build compiles them into the bundles. The SSR bundle
// has the chrome and its imports, and the same modules are reachable from `dist-ssr/`'s chunk graph, so this
// imports the *site* bundle where it can and the SSR bundle for the rest. `vite.ssr.config.ts` keeps `react`
// external so both are importable under Node.

const chrome = await import(pathToFileURL(resolve(DIST_SSR, "ssr-chrome.js")).href);
const preferences = await import(pathToFileURL(resolve(DIST_SSR, "preferences.js")).href);
const samples = await import(pathToFileURL(resolve(DIST_SSR, "sample-folder.js")).href);

const MARKUP = chrome.renderChrome();

// --- the cases ---------------------------------------------------------------------------------------

await test("the built chrome renders the page's four regions", async () => {
  assert.ok(MARKUP.length > 1000, "the chrome rendered something");
  for (const id of ["menubar-file", "device-chip", "restart-app", "editor-host"]) {
    assert.ok(MARKUP.includes(`id="${id}"`), `the chrome carries #${id}`);
  }
  // Issue #431: the two-pane split became a three-column workspace -- the project tree, the tabbed code
  // column and the preview column -- with the status line spanning the bottom. The old `class="layout"`
  // assertion is what caught the move, so it is replaced rather than dropped.
  assert.match(MARKUP, /class="workspace"/, "the three-column workspace is rendered");
  assert.match(MARKUP, /role="tree"/, "the project tree is rendered");
  assert.match(MARKUP, /role="tablist"[^>]*aria-label="Open files"/, "the editor's tabs are rendered");
  assert.match(MARKUP, /data-panel="canvas"/, "the Canvas panel is rendered (W5: the dock replaced the preview switch)");
  assert.match(MARKUP, /class="page-footer"/, "the status line is rendered");
  // The page itself is not a tablist any more: the two strips above and the bottom dock's (Platform
  // events, Mock console, Call trace), and no page-level one.
  assert.equal(
    (MARKUP.match(/role="tablist"/g) || []).length,
    1,
    "the static page has the editor's tab strip and no page-level one (the dock's strips are browser-only)",
  );
});

await test("the menu bar replaced the toolbar's selects and carries the documented option sets", async () => {
  // Issue #557: the five `<select>`s are Device and View menu items now; the rendered chrome has none, and
  // the menu bar, the toolbar and the device chip are in their place.
  assert.equal((MARKUP.match(/<select\b/g) || []).length, 0, "no <select> is left in the chrome");
  assert.match(MARKUP, /role="menubar"[^>]*aria-label="Application menu"/, "the menu bar is rendered");
  assert.equal((MARKUP.match(/class="menubar-item/g) || []).length, 7, "File, Edit, View, Go, Run, Device, Help");
  assert.match(MARKUP, /role="toolbar"[^>]*aria-label="Studio actions"/, "the toolbar is rendered");
  assert.match(MARKUP, /id="device-chip"[^>]*aria-haspopup="menu"/, "the device chip opens the Device menu");
  // The option sets the menus draw from are the same vocabularies the selects used. The skin set is the
  // bundled skins section 5.2 names, in the spec's order (the menu offers the *validated* set).
  assert.deepEqual([...preferences.SKIN_IDS], [
    "",
    "generic-desktop-windows",
    "generic-desktop-macos",
    "generic-desktop-linux",
    "generic-phone-android",
    "generic-phone-ios",
    "generic-tablet",
    "generic-foldable",
  ]);
  assert.deepEqual(
    preferences.VIEWPORTS.map(([width, height]) => `${width}x${height}`),
    ["320x240", "390x844", "768x1024", "1280x800"],
    "four viewports",
  );
  assert.deepEqual([...preferences.DPRS], [1, 2, 3]);
  assert.deepEqual([...preferences.THEMES], ["system", "light", "dark"]);
  assert.deepEqual([...preferences.FOLD_POSTURES], ["", "flat", "book", "tabletop"]);
});

await test("the toolbar persists to localStorage, and the source never does", async () => {
  const { storage } = installBrowser();
  preferences.rememberLocally({
    viewport: "390x844",
    dpr: "2",
    theme: "dark",
    foldPosture: "book",
    skin: "generic-phone-android",
  });
  const stored = JSON.parse(storage.get(STORAGE_KEY));
  assert.equal(stored.viewport, "390x844", "the toolbar position is remembered");
  assert.equal(stored.dpr, "2");
  assert.equal(stored.theme, "dark");
  assert.equal(stored.foldPosture, "book", "the fold posture is remembered too");
  assert.equal(
    stored.skin,
    "generic-phone-android",
    "the skin is remembered too (issue #171's skins slice)",
  );
  assert.equal(stored.source, undefined, "the source is never persisted by the chrome");
});

await test("a fresh page reads no stored preferences and uses its default", async () => {
  installBrowser();
  const state = await preferences.initialState({
    source: "S",
    file: "ui/Preview.tsx",
    width: 320,
    height: 240,
    dpr: 1,
    theme: "system",
    viewport: "320x240",
  });
  assert.equal(state.from, "default");
  assert.equal(state.width, 320);
  assert.equal(state.dpr, 1);
});

await test("a stored toolbar position is restored without the source", async () => {
  const { storage } = installBrowser();
  storage.set(
    STORAGE_KEY,
    JSON.stringify({ viewport: "768x1024", dpr: "3", theme: "light", source: "stale" }),
  );
  const state = await preferences.initialState({
    source: "S",
    file: "ui/Preview.tsx",
    width: 320,
    height: 240,
    dpr: 1,
    theme: "system",
    viewport: "320x240",
  });
  assert.equal(state.from, "local");
  assert.equal(state.width, 768);
  assert.equal(state.height, 1024);
  assert.equal(state.dpr, 3);
  assert.equal(state.theme, "light");
  assert.equal(state.source, "S", "the source comes from the page, never from a previous session");
});

await test("a share link in the address bar is ignored: the studio reads no URL state", async () => {
  // The Share feature was removed (no cloud storage to share to): a leftover `#p5=` fragment is neither decoded
  // nor reported, and `preferences` no longer exports anything that writes a link.
  installBrowser({ hash: "#p5=uAAAA" });
  const state = await preferences.initialState({
    source: "S",
    file: "ui/Preview.tsx",
    width: 320,
    height: 240,
    dpr: 1,
    theme: "system",
    viewport: "320x240",
  });
  assert.equal(state.from, "default");
  assert.equal(state.source, "S");
  for (const name of ["shareStateOf", "shareToLocation", "linkProblemMessage", "ShareError"]) {
    assert.equal(preferences[name], undefined, `${name} is gone`);
  }
});

await test("a picked folder's sample entries: the repository root, samples/<name>, or an ordinary project", async () => {
  const component = (name) => `export function ${name}() { return <View/>; }\n`;
  const root = [
    { path: "README.md", text: "" },
    { path: "designer/web/src/App.tsx", text: component("App") },
    { path: "packages/tessera-ui/Button.tsx", text: component("Button") },
    { path: "samples/gallery/Gallery.tsx", text: component("Gallery") },
    { path: "samples/gallery/helpers.tsx", text: "export function helper() {}\n" },
    { path: "samples/showcase/Showcase.tsx", text: component("Showcase") },
  ];
  assert.deepEqual(samples.sampleEntries(root), ["samples/gallery/Gallery.tsx", "samples/showcase/Showcase.tsx"]);
  const folder = [
    { path: "Gallery.tsx", text: component("Gallery") },
    { path: "FormPage.tsx", text: component("FormPage") },
    { path: "main.cpp", text: "" },
  ];
  assert.deepEqual(samples.sampleEntries(folder), ["FormPage.tsx", "Gallery.tsx"]);
  const project = [
    { path: "tessera.json", text: JSON.stringify({ app: { entry: "src/App.tsx" } }) },
    { path: "src/App.tsx", text: component("App") },
  ];
  assert.deepEqual(samples.sampleEntries(project), [], "a project naming its entry opens as it is");
  // samples/parity-demo has a tessera.json without app.entry: still a sample, and its fields are kept.
  const parity = [
    { path: "tessera.json", text: JSON.stringify({ schemaVersion: 1, app: { namespace: "parity_demo" } }) },
    { path: "Counter.tsx", text: component("Counter") },
  ];
  assert.deepEqual(samples.sampleEntries(parity), ["Counter.tsx"]);
  const config = samples.sampleProject(parity, "Counter.tsx").files.find((file) => file.path === "tessera.json");
  assert.deepEqual(JSON.parse(config.text).app, { name: "counter", namespace: "parity_demo", entry: "Counter.tsx" });
});

await test("the repository root: a library import is copied next to the sample from packages/tessera-ui", async () => {
  const tree = [
    { path: "packages/tessera-ui/Button.tsx", text: 'import "./button.css";\nexport function Button() {}\n' },
    { path: "packages/tessera-ui/button.css", text: ".b {}" },
    { path: "samples/gallery/Gallery.tsx", text: '// import "./Nope"\nimport { Button } from "./Button";\nexport function Gallery() {}\n' },
    { path: "samples/gallery/DashPage.tsx", text: "export function DashPage() {}\n" },
  ];
  const project = samples.sampleProject(tree, "samples/gallery/Gallery.tsx");
  assert.deepEqual(project.files.map((file) => file.path), [
    "samples/gallery/Button.tsx",
    "samples/gallery/Gallery.tsx",
    "samples/gallery/button.css",
    "tessera.json",
  ]);
  assert.deepEqual(project.companions, [
    { path: "samples/gallery/Button.tsx", from: "packages/tessera-ui/Button.tsx" },
    { path: "samples/gallery/button.css", from: "packages/tessera-ui/button.css" },
  ]);
  assert.deepEqual(project.missing, [], "a commented-out import is not an import");
  const config = JSON.parse(project.files.find((file) => file.path === "tessera.json").text);
  assert.equal(config.app.entry, "samples/gallery/Gallery.tsx");
  assert.equal(config.app.namespace, "gallery");
});

await test("samples/<name> alone: a missing library file is named with its repository path", async () => {
  const tree = [
    { path: "Gallery.tsx", text: 'import { Button } from "./Button";\nimport { X } from "../x";\nexport function Gallery() {}\n' },
    { path: "form.module.css", text: "" },
  ];
  const project = samples.sampleProject(tree, "Gallery.tsx");
  assert.deepEqual(project.missing, [
    { importer: "Gallery.tsx", specifier: "../x", path: "../x.tsx", library: null },
    { importer: "Gallery.tsx", specifier: "./Button", path: "Button.tsx", library: "packages/tessera-ui/Button.tsx" },
  ]);
  const sentence = samples.missingSentence(project.missing);
  assert.match(sentence, /Gallery\.tsx imports \.\/Button, which is not in the opened folder/);
  assert.match(sentence, /packages\/tessera-ui\/Button\.tsx in the repository/);
  assert.match(sentence, /Open the repository root/);
  // A sibling in the folder wins over the library, as in the native build's copy.
  const own = samples.sampleProject(
    [...tree, { path: "Button.tsx", text: "export function Button() {}\n" }, { path: "packages/tessera-ui/Button.tsx", text: "LIB" }],
    "Gallery.tsx",
  );
  assert.deepEqual(own.companions, []);
  assert.equal(own.files.find((file) => file.path === "Button.tsx").text, "export function Button() {}\n");
});

process.stdout.write(`\n${failures === 0 ? "studio smoke: all cases passed" : `${failures} failed`}\n`);
if (failures > 0) {
  process.exitCode = 1;
}
