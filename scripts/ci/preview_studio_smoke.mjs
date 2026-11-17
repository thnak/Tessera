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
//      persistence, the fragment decode and the share link) and `designer/web/src/lib/gallery.ts` (the lazy
//      manifest and the sample fetch) have no DOM beyond `localStorage`/`location`/`navigator`, both of which
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
//   * the initial state comes from a fragment first, then storage, then the page's default;
//   * a share link's fragment is produced, put in the address bar, and decoded back to the same state;
//   * a broken fragment shows the link banner with its code, instead of silently loading the default;
//   * the gallery fetches nothing until it is opened, then the manifest once, then the chosen sample.

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

// `preferences.ts` and `gallery.ts` are TypeScript; the build compiles them into the bundles. The SSR bundle
// has the chrome and its imports, and the same modules are reachable from `dist-ssr/`'s chunk graph, so this
// imports the *site* bundle where it can and the SSR bundle for the rest. `vite.ssr.config.ts` keeps `react`
// external so both are importable under Node.

const chrome = await import(pathToFileURL(resolve(DIST_SSR, "ssr-chrome.js")).href);
const preferences = await import(pathToFileURL(resolve(DIST_SSR, "preferences.js")).href);
const gallery = await import(pathToFileURL(resolve(DIST_SSR, "gallery.js")).href);
// The codec itself, imported from **source** rather than from the bundle -- the same files the page imports
// and the share round-trip test checks (ADR-054's risk note). A smoke test that decoded through a bundled
// copy would not be testing what ships.
const codec = await import(
  new URL("../../designer/web/src/lib/share.js", import.meta.url).href
);

const MARKUP = chrome.renderChrome();

// --- the cases ---------------------------------------------------------------------------------------

await test("the built chrome renders the page's four regions", async () => {
  assert.ok(MARKUP.length > 1000, "the chrome rendered something");
  for (const id of ["page-hint", "controls", "source", "gallery", "link-note", "build-id"]) {
    assert.ok(MARKUP.includes(`id="${id}"`), `the chrome carries #${id}`);
  }
  // Issue #431: the two-pane split became a three-column workspace -- the project tree, the tabbed code
  // column and the preview column -- with the status line spanning the bottom. The old `class="layout"`
  // assertion is what caught the move, so it is replaced rather than dropped.
  assert.match(MARKUP, /class="workspace"/, "the three-column workspace is rendered");
  assert.match(MARKUP, /role="tree"/, "the project tree is rendered");
  assert.match(MARKUP, /role="tablist"[^>]*aria-label="Open files"/, "the editor's tabs are rendered");
  assert.match(MARKUP, /role="tablist"[^>]*aria-label="Preview views"/, "the preview switch is rendered");
  assert.match(MARKUP, /class="page-footer"/, "the status line is rendered");
  // The page itself is not a tablist any more: exactly the two strips above, and no third.
  assert.equal(
    (MARKUP.match(/role="tablist"/g) || []).length,
    2,
    "the page has two tab strips (files, preview) and no page-level one",
  );
});

await test("the toolbar's selects carry the documented option sets", async () => {
  const selects = [...MARKUP.matchAll(/<select\b[^>]*id="(control-[^"]+)"[^>]*>([\s\S]*?)<\/select>/g)];
  assert.equal(selects.length, 4, "viewport, DPR, theme, fold posture");
  const values = selects.map(([, id, body]) => ({
    id,
    options: [...body.matchAll(/<option[^>]*value="([^"]*)"/g)].map((match) => match[1]),
  }));
  assert.deepEqual(
    values.find((entry) => entry.id === "control-viewport").options,
    ["320x240", "390x844", "768x1024", "1280x800"],
    "four viewports",
  );
  assert.deepEqual(values.find((entry) => entry.id === "control-dpr").options, ["1", "2", "3"]);
  assert.deepEqual(values.find((entry) => entry.id === "control-theme").options, [
    "system",
    "light",
    "dark",
  ]);
  // The fold posture (issue #408). The option set is the HAL's, in its order: `""` is no fold, and
  // the three named postures are the ones `set_viewport` and the share codec accept.
  assert.deepEqual(values.find((entry) => entry.id === "control-foldPosture").options, [
    "",
    "flat",
    "book",
    "tabletop",
  ]);
});

await test("the toolbar persists to localStorage, and the source never does", async () => {
  const { storage } = installBrowser();
  preferences.rememberLocally({
    viewport: "390x844",
    dpr: "2",
    theme: "dark",
    foldPosture: "book",
  });
  const stored = JSON.parse(storage.get(STORAGE_KEY));
  assert.equal(stored.viewport, "390x844", "the toolbar position is remembered");
  assert.equal(stored.dpr, "2");
  assert.equal(stored.theme, "dark");
  assert.equal(stored.foldPosture, "book", "the fold posture is remembered too");
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

await test("the share link round-trips through the address bar", async () => {
  const { historyCalls } = installBrowser();
  const source = "export function Counter(): Node { return <View />; }\n";
  const state = preferences.shareStateOf(source, {
    viewport: "1280x800",
    dpr: "1",
    theme: "system",
    foldPosture: "tabletop",
  });
  const url = await preferences.shareToLocation(state);
  assert.match(url, /#p5=/, "a fragment link was produced");
  assert.ok(historyCalls.includes(url), "the address bar was rewritten");
  const back = await codec.decodeShare(url);
  assert.equal(back.source, source, "the source round-trips");
  assert.equal(back.width, 1280);
  assert.equal(back.height, 800);
  assert.equal(back.dpr, 1);
  assert.equal(back.theme, "system");
  // A preview of a foldable screen is not the same preview without its posture, so the link carries
  // it (issue #408) and this is where a page that forgot to pass it fails.
  assert.equal(back.foldPosture, "tabletop", "the fold posture round-trips through the link");
});

await test("opening a share link restores its source, viewport and theme", async () => {
  const shared = {
    source: "export function X(): Node { return <Text>hi</Text>; }\n",
    file: "ui/Preview.tsx",
    width: 768,
    height: 1024,
    dpr: 3,
    theme: "dark",
  };
  const payload = await codec.encodeShare(shared);
  installBrowser({ hash: `#p5=${payload}` });
  const state = await preferences.initialState({
    source: "ignored",
    file: "ui/Preview.tsx",
    width: 320,
    height: 240,
    dpr: 1,
    theme: "system",
    viewport: "320x240",
  });
  assert.equal(state.from, "link");
  assert.equal(state.source, shared.source);
  assert.equal(state.width, 768);
  assert.equal(state.height, 1024);
  assert.equal(state.dpr, 3);
  assert.equal(state.theme, "dark");
});

await test("a broken share link is reported with its code, and the default source survives", async () => {
  installBrowser({ hash: "#p5=zAAAA" });
  let message = null;
  try {
    await preferences.initialState({
      source: "S",
      file: "ui/Preview.tsx",
      width: 320,
      height: 240,
      dpr: 1,
      theme: "system",
      viewport: "320x240",
    });
    assert.fail("a malformed link must throw so the page can show the banner");
  } catch (error) {
    message = preferences.linkProblemMessage(error);
  }
  assert.match(message, /could not be opened/);
  assert.match(message, /malformed/, "the ShareError code is shown");
});

await test("the gallery fetches nothing until it is asked to, then the manifest once", async () => {
  const fetched = [];
  const MANIFEST = {
    schema: 1,
    samples: [
      { path: "samples/parity-demo/Counter.tsx", component: "Counter" },
      { path: "samples/gallery/Gallery.tsx", component: "Gallery" },
    ],
  };
  const fetchImpl = async (url) => {
    fetched.push(url);
    if (url === "./samples.json") {
      return { ok: true, status: 200, json: async () => MANIFEST };
    }
    if (url.endsWith(".tsx")) {
      return { ok: true, status: 200, text: async () => "SAMPLE" };
    }
    return { ok: false, status: 404, text: async () => "" };
  };
  const cache = { samples: null };
  assert.deepEqual(fetched, [], "nothing is fetched before the gallery opens");
  const first = await gallery.cachedManifest(cache, fetchImpl);
  assert.deepEqual(fetched, ["./samples.json"], "the manifest is fetched on first open");
  assert.equal(first.length, 2, "one entry per sample");
  await gallery.cachedManifest(cache, fetchImpl);
  assert.equal(
    fetched.filter((url) => url === "./samples.json").length,
    1,
    "the manifest is fetched once per page load",
  );
  const text = await gallery.loadSampleSource("samples/parity-demo/Counter.tsx", fetchImpl);
  assert.equal(text, "SAMPLE");
  assert.equal(gallery.titleOf("samples/parity-demo/Counter.tsx"), "Counter (parity-demo)");
});

await test("a failed sample fetch is an error the gallery can show, not a thrown page", async () => {
  const fetchImpl = async () => ({ ok: false, status: 404, text: async () => "" });
  await assert.rejects(
    () => gallery.loadSampleSource("samples/missing/Missing.tsx", fetchImpl),
    /404/,
  );
});

await test("the manifest is validated, not trusted", async () => {
  const unsafe = async () => ({
    ok: true,
    status: 200,
    json: async () => ({ schema: 1, samples: [{ path: "../escape.tsx", component: "X" }] }),
  });
  await assert.rejects(() => gallery.loadSampleManifest(unsafe), /unsafe path/);
  const wrongSchema = async () => ({
    ok: true,
    status: 200,
    json: async () => ({ schema: 2, samples: [] }),
  });
  await assert.rejects(() => gallery.loadSampleManifest(wrongSchema), /schema 2/);
});

process.stdout.write(`\n${failures === 0 ? "studio smoke: all cases passed" : `${failures} failed`}\n`);
if (failures > 0) {
  process.exitCode = 1;
}
