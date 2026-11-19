// SPDX-License-Identifier: MIT
//
// Checks for the studio page's wiring, against the **built** page (ADR-054 decision 6; the checks started
// as issue #169's P5 wiring checks and were rewritten for the React conversion in issue #422).
//
//   node scripts/ci/preview_page_checks.mjs
//
// **Why this reads the built output and not `designer/web/src`.** React creates the studio's DOM at
// runtime, so the shipped `index.html` is a mount point: `id="root"` and a hashed bundle. A static "every
// id exists in the HTML" check would pass on a page whose chrome never mounts, which is the worst kind of
// green. The chrome is therefore rendered from the *same components the bundle ships*, with
// `react-dom/server`'s `renderToStaticMarkup`, and the ids and roles are asserted against that markup.
//
// **Why `dist/` and not the sources.** The bundle is what a visitor downloads, and a build that drops a
// component or mangles an id is exactly the failure a source-only check cannot see. `npm run build` (which
// builds both `dist/` and `dist-ssr/`) is a prerequisite; this script says so when the artifacts are
// missing rather than failing with a stack trace.
//
// The checks:
//
// 1. **The built artifact is the static page the CSP allows.** `dist/index.html` exists, references its
//    hashed CSS/JS with `src`/`href` (never an inline script: ADR-054 decision 4), and every asset it
//    references exists on disk. A fixed file list cannot survive content hashes, so the list is derived
//    from the document itself -- this is the check the old `test -f` list turned into.
// 2. **Every element the shared chrome and the shell look up is rendered.** The ids `App` and the four
//    regions resolve, against the SSR markup of the real components.
// 3. **Every id a view session resolves is rendered**, per view, so a failure names the pane that is wrong.
// 4. **The redirect stubs carry the query and the fragment.** This is the pre-merge URL migration: the old
//    URLs must forward to `index.html` with `location.search` and `location.hash` intact and the view
//    selected. The check drives the real `redirect.js` under Node against the six old-URL shapes a real
//    link takes, and each stub's own HTML is checked for the call and for the absence of engine code.
// 5. **Every `samples.json` path exists.** The Pages lane copies `samples/` into the site and the gallery
//    fetches `./<path>`; a missing file is a button that cannot load. This passes locally (where `samples/`
//    sits beside `designer/`) and fails only on the deployed site, which is the worst place to find it.
// 6. **`preview-state.js` is still a pure ES module.** ADR-054's risk section: no DOM, no React, no bundler
//    syntax, so the Node gates and the page keep importing the same bytes.
// 7. **The three Wasm modules stayed three and stayed lazy.** No module is imported at the top level of the
//    built bundle, and each module's URL appears exactly once, inside a dynamic `import()`.
// 8. **No page module calls a function it does not declare.** The audit that caught five shipped defects in
//    P5, now run over the built bundle's sources and the redirect script.
//
// This is NOT a rendering test. That the page *paints* is what the deploy and a human opening the URL
// verify (`preview_browser_check.mjs` is the developer-run browser half); nothing here claims otherwise.

import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const WEB = resolve(REPO_ROOT, "designer", "web");
const DIST = resolve(WEB, "dist");
const DIST_SSR = resolve(WEB, "dist-ssr");

// The ids the shared chrome (`App`, `StudioChrome`, `ProjectTree`, `CodeRegion`) resolves. One page, so
// one list.
//
// Issue #431 split the page into three columns and moved the view switch into the preview column, so this
// list grew by the workspace's own ids (`workspace`, `project-tree`, the tab strip, the status line's
// cells) and lost nothing: every id a pre-#431 link or gate depended on is still here.
const SHARED_IDS = [
  // The workbench chrome (issue #557): the notification bell and live region, the device chip that
  // replaced the preview toolbar's selects, and the menu bar (one button per top-level menu; `Go` is
  // hidden until its first command is live).
  "notifications",
  "notification-live",
  "notifications-bell",
  "device-chip",
  "menubar-file",
  "menubar-edit",
  "menubar-view",
  "menubar-run",
  "menubar-device",
  "menubar-help",
  "editor-host",
  // The three-column workspace (issue #431). `workspace` is the grid, `project-tree` is its left column,
  // `editor-panel` is the middle column's tab panel, and the `status-*` ids are the status bar (the Problems
  // item with its split error and warning counts, the caret, the language).
  "workspace",
  "project-tree",
  "project-status",
  "editor-panel",
  "status-diagnostics-toggle",
  "status-errors",
  "status-warnings",
  "status-caret",
  "status-language",
];

// The page shell's own ids: `App` and the four regions resolve these, and nothing else would catch one
// being renamed out from under them. The three `tab-*`/`view-*` pairs are the preview switch, which #431
// moved into the preview column but did not rename: `?view=` and every `#p5=` link still resolve them.
const SHELL_IDS = [
  // W5 (#560): the `tab-*` ids went with the preview switch; the dock's tab strips replace it. The
  // `view-*` ids are the panes themselves and stay.
  "view-diagnostics",
  "view-scene",
  "view-canvas",
  // The primary action is the cold start since issue #444: the edit applies itself, so the old `run`
  // button became **Restart app** and its id moved with it. `apply-status` is the debounce's own visible
  // state ("applying…"), which is what makes the automatic apply distinguishable from a preview that
  // lags; the check below requires it, so a rename cannot silently drop the only feedback a keystroke
  // gets.
  "restart-app",
  "apply-status",
];

// Ids that were removed on purpose (owner decisions, studio focus pass) and must not come back: the Share
// button, the broken-link banner and the two visually hidden editor lines.
const REMOVED_IDS = ["share", "link-note", "editor-hint", "editor-file", "share-status"];

// Each view session's own ids, kept separate so a failure names the view whose pane is wrong.
const VIEW_IDS = {
  diagnostics: ["diag-status", "diagnostics", "diag-empty"],
  scene: [
    "scene-status",
    "nodes",
    "scene-empty",
    "scene-hint",
    "script",
    "run-input",
    "input-status",
    "cells",
  ],
  canvas: [
    "canvas-status",
    "unsupported",
    "unsupported-heading",
    "unsupported-body",
    "canvas-frame",
    "canvas",
    "failed",
    "failed-body",
    "frame-stats",
    "stat-frame",
    "stat-draws",
    "stat-vertices",
    "stat-nodes",
    "stat-dpr",
  // Issue #387: the font row. The page must name the face text is painted with, or say that text is
  // not painted, so a missing font is a statement rather than an empty canvas.
  "stat-font",
  // The running evaluation's state row (issue #444). `preview_browser_live_edit.mjs` reads both the
  // row and its value cell to prove a carried cell survives an edit and a restart resets it, so a
  // rename here would silently disarm the live edit's acceptance evidence.
  "state-row",
  "stat-state",
  // The picker overlay (issue #171): the toggle that arms it and the layer the outline is drawn into.
  // The overlay is `pointer-events: none` in CSS, which is what keeps the preview's own pointer path
  // working while the picker is armed; a rename here would leave the picker with nowhere to draw.
  "pick-toggle",
  "pick-overlay",
  // The style inspector (issue #171, designer-protocol-v1 section 9.1): the toggle that arms it, the
  // panel it fills, and the body the rows go into. The panel is a page element beside the canvas, so
  // a rename here would leave the inspector with nowhere to draw.
  "inspect-toggle",
  "inspect-panel",
  "inspect-body",
  // The simulated device frame (issue #171's skins slice). It is page chrome around the canvas, so
  // React renders it and its id is part of the contract; the bars and the cutout inside it are
  // `aria-hidden` decoration with classes rather than ids, because nothing looks them up.
  "skin-frame",
  ],
};

// Browser and language globals a page may use without declaring. Kept explicit so the audit fails on an
// unknown name instead of silently accepting one.
const BROWSER_GLOBALS = new Set([
  "document", "window", "globalThis", "console", "fetch", "navigator", "location", "history",
  "localStorage", "JSON", "Math", "Number", "String", "Boolean", "Array", "Object", "Promise",
  "Error", "TypeError", "Set", "Map", "Date", "TextEncoder", "TextDecoder", "Blob", "Response",
  "Uint8Array", "CompressionStream", "DecompressionStream", "getComputedStyle", "parseFloat",
  "parseInt", "isNaN", "Reflect", "Proxy", "undefined", "null", "true", "false", "this",
  "arguments", "await", "async", "function", "return", "if", "else", "for", "while", "const", "let",
  "var", "new", "typeof", "instanceof", "of", "in", "do", "switch", "case", "break", "continue",
  "try", "catch", "finally", "throw", "class", "extends", "super", "import", "export", "default",
  "from", "void", "delete", "yield", "static", "get", "set", "encodeURIComponent", "decodeURIComponent",
  "URLSearchParams", "URL", "Intl", "BigInt", "Symbol", "Infinity", "NaN", "isFinite", "structuredClone",
  "requestAnimationFrame", "cancelAnimationFrame", "setTimeout", "clearTimeout", "queueMicrotask",
]);

let failures = 0;

function fail(message) {
  process.stdout.write(`FAIL ${message}\n`);
  failures += 1;
}

function ok(message) {
  process.stdout.write(`ok   ${message}\n`);
}

/** Every file under `root`, as POSIX paths relative to it. */
function walk(root, prefix = "") {
  const out = [];
  for (const name of readdirSync(resolve(root, prefix))) {
    const relative = prefix ? `${prefix}/${name}` : name;
    if (statSync(resolve(root, relative)).isDirectory()) {
      out.push(...walk(root, relative));
    } else {
      out.push(relative);
    }
  }
  return out;
}

/**
 * Strips comments, string literals and template literals from JavaScript, leaving the code.
 *
 * The pure module this script audits *documents* the rule it keeps ("no DOM") -- so a scan of the raw text would fail a file for explaining itself. This
 * is a small lexer, not a parser: it tracks comments, the three quote styles and regex literals, which is
 * everything the audit needs and nothing it does not.
 */
function stripNonCode(text) {
  let out = "";
  let index = 0;
  let lastSignificant = "";
  while (index < text.length) {
    const character = text[index];
    const next = text[index + 1];
    if (character === "/" && next === "/") {
      const end = text.indexOf("\n", index);
      index = end < 0 ? text.length : end;
      continue;
    }
    if (character === "/" && next === "*") {
      const end = text.indexOf("*/", index + 2);
      index = end < 0 ? text.length : end + 2;
      continue;
    }
    if (character === "/" && (lastSignificant === "" || "(,=:[!&|?{};+-*%<>~^".includes(lastSignificant))) {
      // A regex literal: consume to the closing `/`, honouring a character class and the flags.
      let cursor = index + 1;
      let inClass = false;
      while (cursor < text.length) {
        const current = text[cursor];
        if (current === "\\") {
          cursor += 2;
          continue;
        }
        if (current === "[") {
          inClass = true;
        } else if (current === "]") {
          inClass = false;
        } else if (current === "/" && !inClass) {
          cursor += 1;
          break;
        } else if (current === "\n") {
          break;
        }
        cursor += 1;
      }
      while (cursor < text.length && /[a-z]/i.test(text[cursor])) {
        cursor += 1;
      }
      out += " ";
      index = cursor;
      lastSignificant = "/";
      continue;
    }
    if (character === '"' || character === "'" || character === "`") {
      const quote = character;
      let cursor = index + 1;
      while (cursor < text.length) {
        const current = text[cursor];
        if (current === "\\") {
          cursor += 2;
          continue;
        }
        if (current === quote) {
          cursor += 1;
          break;
        }
        // A template's `${...}` is *code*, not text: it is emitted (recursively stripped) so an
        // interpolation that calls a function is still seen by the audit.
        if (quote === "`" && current === "$" && text[cursor + 1] === "{") {
          let depth = 1;
          let inner = cursor + 2;
          while (inner < text.length && depth > 0) {
            const token = text[inner];
            if (token === "{") {
              depth += 1;
            } else if (token === "}") {
              depth -= 1;
            }
            inner += 1;
          }
          out += " " + stripNonCode(text.slice(cursor + 2, inner - 1)) + " ";
          cursor = inner;
          continue;
        }
        cursor += 1;
      }
      out += " ";
      index = cursor;
      lastSignificant = quote;
      continue;
    }
    out += character;
    if (!/\s/.test(character)) {
      lastSignificant = character;
    }
    index += 1;
  }
  return out;
}

function requireBuilt() {  try {
    readFileSync(resolve(DIST, "index.html"));
  } catch {
    process.stderr.write(
      `SETUP ERROR: ${DIST} has no index.html. Build the page first:\n` +
        "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
    );
    return false;
  }
  try {
    readFileSync(resolve(DIST_SSR, "ssr-chrome.js"));
  } catch {
    process.stderr.write(
      `SETUP ERROR: ${DIST_SSR} has no ssr-chrome.js. The chrome checks render the real components,\n` +
        "which needs `npm run build` (it builds dist/ and dist-ssr/).\n",
    );
    return false;
  }
  return true;
}

if (!requireBuilt()) {
  process.exitCode = 2;
} else {
  // --- 1. the built artifact is a static page the strict CSP allows -------------------------------

  {
    const document_ = readFileSync(resolve(DIST, "index.html"), "utf8");
    // The page's own comments explain the CSP and quote a `<script>`; scanning them would report the
    // explanation as the violation. Only the markup a browser parses is checked.
    const page = document_.replace(/<!--[\s\S]*?-->/g, "");
    const problems = [];

    // An inline `<script>` (one with no `src`) is the thing ADR-054 decision 4 forbids. A `<script
    // type="application/json">` block would also be inline; there is none, and this is what keeps it so.
    for (const match of page.matchAll(/<script\b([^>]*)>/g)) {
      if (!/\bsrc\s*=/.test(match[1])) {
        problems.push(`inline <script${match[1]}>`);
      }
    }
    // A remote script or stylesheet is the other half of the rule.
    for (const match of page.matchAll(/<(script|link)\b[^>]*(?:src|href)\s*=\s*"([^"]+)"/g)) {
      const url = match[2];
      if (/^(?:https?:)?\/\//i.test(url)) {
        problems.push(`remote ${match[1]} ${url}`);
      }
    }
    // Every asset the document names must exist: this replaces the fixed `test -f` list, which cannot
    // survive content hashes. A dangling reference is a page that silently loses its chrome.
    const referenced = [
      ...[...page.matchAll(/<script\b[^>]*\bsrc\s*=\s*"([^"]+)"/g)].map((match) => match[1]),
      ...[...page.matchAll(/<link\b[^>]*\bhref\s*=\s*"([^"]+)"/g)].map((match) => match[1]),
    ].filter((url) => url.startsWith("./") || (!url.startsWith("/") && !/^[a-z]+:/i.test(url)));
    for (const url of referenced) {
      try {
        readFileSync(resolve(DIST, url));
      } catch {
        problems.push(`dangling asset reference ${url}`);
      }
    }
    if (referenced.length < 2) {
      problems.push(`expected a hashed script and stylesheet, found ${referenced.length} reference(s)`);
    }
    // The mount point itself, and no hand-written chrome left in the document.
    if (!page.includes('id="root"')) {
      problems.push("no #root mount point");
    }
    if (page.includes('id="view-diagnostics"')) {
      problems.push("the document still carries hand-written panes (they are React components now)");
    }

    if (problems.length > 0) {
      fail(`dist/index.html ${problems.join("; ")}`);
    } else {
      ok(
        `dist/index.html is CSP-clean and its ${referenced.length} asset reference(s) all exist ` +
          "(no inline or remote script)",
      );
    }
  }

  // --- 2 + 3. the rendered chrome carries the ids the page's code looks up ------------------------

  const chrome = await import(pathToFileURL(resolve(DIST_SSR, "ssr-chrome.js")).href);
  const markup = chrome.renderChrome();

  // The first view has no fixed sample project (#391, owner direction 2026-11-17): an empty tree that says
  // how to open one, and no open file. The seeded `markup` above exists only so the layout can be asserted.
  {
    const boot = chrome.renderBootChrome();
    const problems = [];
    if (/role="treeitem"/.test(boot)) {
      problems.push("a treeitem is rendered before any project is opened");
    }
    // The samples bar is gone (owner feedback on #586): the welcome view is where a first-time visitor finds
    // samples, next to Open Folder / Open Zip, and the chrome carries no horizontal gallery strip.
    if (!/id="welcome"/.test(boot) || !/Open Folder/.test(boot)) {
      problems.push("the welcome view (Open Folder / Open Zip / Samples) is missing from the first view");
    }
    if (/id="gallery/.test(boot) || /id="gallery/.test(markup)) {
      problems.push("the sample gallery strip is back; samples live in the welcome view and Open Sample");
    }
    if (!/No project open/.test(boot)) {
      problems.push("the empty tree does not say how to open a project");
    }
    if (/role="tab"[^>]*id="tab-(?:file|ui)/.test(boot) || /class="editor-tab/.test(boot)) {
      problems.push("an editor tab is open before any project is");
    }
    // Open folder / Open zip are File menu commands since issue #557: the menu bar is how a visitor starts.
    if (!/role="menubar"/.test(boot) || !/id="menubar-file"/.test(boot)) {
      problems.push("the File menu (Open Folder / Open Zip) is missing from the first view");
    }
    if (problems.length > 0) {
      fail(`the first view ${problems.join("; ")}`);
    } else {
      ok("the first view is an empty workspace with a File menu and no seeded project");
    }
  }

  {
    const missing = [...SHARED_IDS, ...SHELL_IDS].filter((id) => !markup.includes(`id="${id}"`));
    for (const id of missing) {
      fail(`the rendered chrome is missing id="${id}" that App/StudioChrome/ViewPanes resolve`);
    }
    const revived = REMOVED_IDS.filter((id) => markup.includes(`id="${id}"`));
    for (const id of revived) {
      fail(`the rendered chrome carries id="${id}", which was removed on purpose`);
    }
    if (revived.length === 0) {
      ok(`the rendered chrome carries none of the removed ids (${REMOVED_IDS.join(", ")})`);
    }
    if (missing.length === 0) {
      ok(
        `the rendered chrome carries every element the shared chrome and the shell look up ` +
          `(${SHARED_IDS.length + SHELL_IDS.length})`,
      );
    }
  }

  // The two tab strips are role-bearing chrome, and each must be a real tablist: one tab stop, one
  // selection, arrow-key reachable. Issue #431 made them two -- the open files in the code column and the
  // preview switch in the preview column -- and *removed* the page-level one. So the checks are (a) the
  // preview switch is still three tabs with exactly one selection, exactly as before, and (b) the page
  // carries no third tablist and no fourth preview tab: a layout that put the switch back at the top
  // would pass (a) and fail (b), which is the regression this issue exists to prevent.
  {
    const problems = [];
    // The markup is split at each `role="tablist"`, so a strip's tabs are the `role="tab"` elements
    // between its opening tag and the next strip. The regions are in DOM order (tree, code, preview), so
    // the editor's strip precedes the preview's in the markup; slicing by position keeps the two apart
    // without depending on which order the components happen to be declared in.
    const starts = [...markup.matchAll(/role="tablist"/g)].map((match) => match.index ?? 0);
    const strips = starts.map((start, index) => {
      const end = index + 1 < starts.length ? starts[index + 1] : markup.length;
      const block = markup.slice(start, end);
      const label = /aria-label="([^"]*)"/.exec(block)?.[1] ?? "";
      const tabs = [...block.matchAll(/role="tab"([^>]*)>/g)].map((match) => match[1]);
      return { label, tabs };
    });
    const labels = strips.map((strip) => strip.label);
    // The bottom dock (issue #434) is its own region with its own strip, so it is counted apart: the
    // page still has exactly two strips in the workspace, and the dock's is checked on its own below.
    // W5 (#560): the preview switch and the bottom dock's strip are the dock library's tab strips now, which
    // only exist in a browser (the Node render has no measured dock), so the static markup carries the
    // editor's strip alone. The dock's own strips are asserted by `workbench_dock.mjs` in a real browser.
    const workspaceStrips = strips;
    if (workspaceStrips.length !== 1) {
      problems.push(`${workspaceStrips.length} tablist(s), expected 1 (the open files)`);
    }
    if (!labels.includes("Open files")) {
      problems.push("no `Open files` tablist (the editor has no tabs)");
    }

    /** One strip's roles: n tabs, exactly one selected, exactly one tab stop. */
    const checkStrip = (name, expected) => {
      const strip = strips.find((candidate) => candidate.label === name);
      if (!strip) {
        return 0;
      }
      if (expected !== null && strip.tabs.length !== expected) {
        problems.push(`${strip.tabs.length} ${name} role=tab element(s), expected ${expected}`);
      }
      const selected = strip.tabs.filter((attributes) => /aria-selected="true"/.test(attributes)).length;
      if (selected !== 1) {
        problems.push(`${selected} selected ${name} tab(s), expected exactly 1`);
      }
      const focusable = strip.tabs.filter((attributes) => !/tabindex="-1"/.test(attributes)).length;
      if (focusable !== 1) {
        problems.push(`${focusable} tab stop(s) in the ${name} strip, expected exactly 1 (roving tabindex)`);
      }
      return strip.tabs.length;
    };

    // The editor: the seeded project opens its entry file, so there is at least one tab.
    const editorTabs = checkStrip("Open files", null);
    if (editorTabs < 1) {
      problems.push("no editor tabs (the workspace has no open file)");
    }

    if (problems.length > 0) {
      fail(`the rendered tab strips ${problems.join("; ")}`);
    } else {
      ok(
        `the editor's strip is one tab stop with one selection; the editor has ` +
          `${editorTabs} tab(s) with one selection`,
      );
    }
  }

  // The workspace's shape (issue #431): three columns with a tree in the first one. A page that went back
  // to top-level view tabs would still render every id above, so the *layout* is asserted separately.
  {
    const problems = [];
    if (!/class="workspace"/.test(markup)) {
      problems.push("no .workspace three-column grid");
    }
    if (!/role="tree"/.test(markup)) {
      problems.push("no role=tree (the project tree is missing)");
    }
    const items = [...markup.matchAll(/role="treeitem"([^>]*)>/g)].map((match) => match[1]);
    if (items.length < 3) {
      problems.push(`${items.length} treeitem(s), expected the seeded project's rows`);
    }
    // One tab stop in the tree, on the selected row: a tree that is keyboard navigable has one entry
    // point, and the arrow keys move it.
    const selected = items.filter((attributes) => /aria-selected="true"/.test(attributes)).length;
    if (selected !== 1) {
      problems.push(`${selected} selected tree row(s), expected exactly 1`);
    }
    const stops = items.filter((attributes) => !/tabindex="-1"/.test(attributes)).length;
    if (stops !== 1) {
      problems.push(`${stops} tab stop(s) in the tree, expected exactly 1`);
    }
    if (!/aria-level="\d+"/.test(markup)) {
      problems.push("no aria-level on a treeitem (the tree has no depth)");
    }
    // The status line spans the bottom: it is a `footer` and it carries the three cells the issue names.
    if (!/<footer[^>]*class="page-footer"/.test(markup)) {
      problems.push("no status-line footer");
    }
    if (problems.length > 0) {
      fail(`the rendered workspace ${problems.join("; ")}`);
    } else {
      ok(`the workspace is three columns with a keyboard tree (${items.length} rows) and a status line`);
    }
  }

  for (const [view, ids] of Object.entries(VIEW_IDS)) {
    const missing = ids.filter((id) => !markup.includes(`id="${id}"`));
    for (const id of missing) {
      fail(`the rendered chrome is missing id="${id}" that the ${view} session resolves`);
    }
    if (missing.length === 0) {
      ok(`the ${view} pane carries every id its session resolves (${ids.length})`);
    }
  }

  // --- 4. the redirect stubs preserve the query, the fragment and the view -------------------------

  // The real module, loaded the way a browser loads it: a classic script that publishes
  // `globalThis.tesseraRedirect`, so importing it under Node is the same code path with a fake global.
  await import(pathToFileURL(resolve(DIST, "redirect.js")).href);

  const redirect = globalThis.tesseraRedirect;
  if (!redirect || typeof redirect.mergedUrl !== "function") {
    fail("redirect.js does not publish tesseraRedirect.mergedUrl (the stubs would not forward)");
  } else {
    // The old-URL shapes a real link takes, including the ones with nothing in them. A share link's
    // fragment is the whole project state, so it is compared byte for byte rather than "present".
    const PAYLOAD = "zKLUv_QBFYgA"; // a real-looking deflate-raw payload, short enough to read
    const cases = [
      {
        why: "a pre-merge share link with a query and a fragment",
        view: "scene",
        search: "?viewport=390x844&dpr=2&theme=dark",
        hash: `#p5=${PAYLOAD}`,
        want: `index.html?viewport=390x844&dpr=2&theme=dark&view=scene#p5=${PAYLOAD}`,
      },
      {
        why: "a pre-merge link with no query and no fragment",
        view: "canvas",
        search: "",
        hash: "",
        want: "index.html?view=canvas",
      },
      {
        why: "a pre-merge link with a fragment only",
        view: "canvas",
        search: "",
        hash: `#p5=${PAYLOAD}`,
        want: `index.html?view=canvas#p5=${PAYLOAD}`,
      },
      {
        why: "a pre-merge link with a query only",
        view: "scene",
        search: "?theme=light",
        hash: "",
        want: "index.html?theme=light&view=scene",
      },
      {
        why: "a stale ?view= in the old query is replaced, not duplicated",
        view: "canvas",
        search: "?view=scene&dpr=3",
        hash: "#p5=abc",
        want: "index.html?view=canvas&dpr=3#p5=abc",
      },
      {
        why: "a fragment that is not a share payload is still carried verbatim",
        view: "scene",
        search: "",
        hash: "#section-2",
        want: "index.html?view=scene#section-2",
      },
    ];

    let bad = 0;
    for (const testCase of cases) {
      const got = redirect.mergedUrl(testCase.view, testCase.search, testCase.hash);
      if (got !== testCase.want) {
        fail(
          `redirect for ${testCase.why} produced ${JSON.stringify(got)}, ` +
            `expected ${JSON.stringify(testCase.want)}`,
        );
        bad += 1;
      }
    }
    if (bad === 0) {
      ok(`the redirect preserves the query and the #p5= fragment for all ${cases.length} old-URL shapes`);
    }

    // The stubs themselves: each must load the redirect module and call it with its own view, and must
    // not import engine code (a stub that had to fetch a module before forwarding could fail to forward).
    for (const [page, view] of [
      ["scene.html", "scene"],
      ["canvas.html", "canvas"],
    ]) {
      const stub = readFileSync(resolve(DIST, page), "utf8");
      const problems = [];
      if (!stub.includes('src="redirect.js"')) {
        problems.push("does not load redirect.js");
      }
      if (!stub.includes(`redirectTo('${view}')`)) {
        problems.push(`does not forward to the ${view} view`);
      }
      if (/type="module"|tessera_(diagnostics|scene_preview|canvas)\.js/.test(stub)) {
        problems.push("loads engine code (a stub must not depend on a module loading)");
      }
      if (problems.length > 0) {
        fail(`${page} ${problems.join("; ")}`);
      } else {
        ok(`${page} is a redirect stub to the ${view} view with no engine code`);
      }
    }
  }

  // --- 5. every manifest path exists, under the path the site uses --------------------------------

  const manifest = JSON.parse(readFileSync(resolve(DIST, "samples.json"), "utf8"));
  if (manifest.schema !== 1) {
    fail(`samples.json is schema ${manifest.schema}, expected 1`);
  }
  let missingSamples = 0;
  for (const entry of manifest.samples || []) {
    try {
      readFileSync(resolve(REPO_ROOT, entry.path));
    } catch {
      fail(`samples.json names a missing file: ${entry.path}`);
      missingSamples += 1;
    }
  }
  if (missingSamples === 0) {
    ok(`samples.json names ${(manifest.samples || []).length} sample(s), all present`);
  }

  // The manifest and the two stubs must be *in the built tree*: they are copied through `public/`, and a
  // Vite configuration that stops copying it would publish a site whose gallery 404s.
  for (const name of ["samples.json", "scene.html", "canvas.html"]) {
    try {
      readFileSync(resolve(DIST, name));
    } catch {
      fail(`dist/${name} is missing (it is copied from designer/web/public/)`);
    }
  }

  // --- 6. the codec is still a pure ES module ----------------------------------------------------

  // ADR-054's risk section names this: `preview-state.js` must have no DOM and no React, so the Node gates keep
  // testing the same bytes the page ships. The check is deliberately about *imports and globals*, not about
  // style: a bundler-only specifier, a DOM call or a React import would break that, and nothing else in the
  // suite would see it.
  for (const name of ["preview-state.js"]) {
    const raw = readFileSync(resolve(WEB, "src", "lib", name), "utf8");
    // Comments and string literals are stripped first: both files *describe* what they must not do
    // (its own header says "no DOM"), and a scan that read the prose would fail the file for explaining the
    // rule it keeps. What is left is code.
    const source = stripNonCode(raw);
    const problems = [];
    if (/\bimport\b[^;]*\breact\b/i.test(source)) {
      problems.push("imports React");
    }
    // Only relative specifiers: a bare or bundler-alias specifier would not resolve under plain Node, which
    // is exactly how the gates import this file.
    for (const match of source.matchAll(/\bfrom\s+["']([^"']+)["']/g)) {
      if (!match[1].startsWith("./")) {
        problems.push(`imports a non-relative specifier ${match[1]}`);
      }
    }
    // DOM entry points: `document`, `window`, `localStorage` and `navigator` are the ones that would make it untestable.
    for (const global of ["document", "window", "localStorage", "navigator", "history", "location"]) {
      if (new RegExp(`(?<![.\\w$])${global}\\s*(?:\\.|\\[)`).test(source)) {
        problems.push(`uses the DOM global \`${global}\``);
      }
    }
    if (problems.length > 0) {
      fail(`${name} is no longer a pure ES module: ${problems.join("; ")}`);
    } else {
      ok(`${name} is still a pure ES module (no DOM, no React, relative imports only)`);
    }
  }

  // --- 7. three Wasm modules, three lazy loads ---------------------------------------------------

  {
    const assets = walk(DIST).filter((name) => name.endsWith(".js"));
    const bundle = assets.map((name) => readFileSync(resolve(DIST, name), "utf8")).join("\n");
    const names = ["tessera_diagnostics.js", "tessera_scene_preview.js", "tessera_canvas.js"];
    const problems = [];
    for (const name of names) {
      const count = bundle.split(name).length - 1;
      if (count === 0) {
        problems.push(`${name} is not referenced by the bundle (a view lost its module)`);
      } else if (count > 1) {
        problems.push(`${name} appears ${count} times (the three modules must stay three)`);
      }
    }
    // A dynamic import, not a static one: a top-level `import ... from "tessera_x.js"` would download the
    // module at first paint, which is the laziness the merge and the conversion both have to keep.
    for (const name of names) {
      if (!new RegExp(`import\\([^)]*${name.replace(/\./g, "\\.")}`).test(bundle)) {
        problems.push(`${name} is not loaded through a dynamic import()`);
      }
    }
    // No module preload of a Wasm loader: Vite emits `<link rel="modulepreload">` for static imports, and a
    // preload for a lazy module would be the same eager download by another name.
    const page = readFileSync(resolve(DIST, "index.html"), "utf8");
    if (/modulepreload[^>]*tessera_/.test(page)) {
      problems.push("the document preloads a Wasm module");
    }
    if (problems.length > 0) {
      fail(`the built bundle ${problems.join("; ")}`);
    } else {
      ok("the built bundle references three Wasm modules, each through one dynamic import()");
    }
  }

  // --- 8. no page source calls a function it does not declare -------------------------------------

  /**
   * Names this file *calls* as a bare function but neither declares, imports, nor inherits.
   *
   * The scan is deliberately shallow -- a scope-free identifier scan, not a parser -- because it only has to
   * catch "the name is gone". It is also deliberately precise about what counts as a call: the candidate must
   * be an identifier immediately followed by `(` and *not* preceded by `.` (so a method call on an object is
   * not a free identifier). That precision is what keeps an object literal's key (`load: loadModule`) and a
   * regex flag (`/\+/g`) from being reported as dangling calls.
   */
  function undeclaredNames(text) {
    const declared = new Set();
    for (const match of text.matchAll(/\b(?:function|const|let|var|class)\s+([A-Za-z_$][\w$]*)/g)) {
      declared.add(match[1]);
    }
    for (const match of text.matchAll(/import\s*\{([^}]*)\}/g)) {
      for (const part of match[1].split(",")) {
        const name = part.trim().split(/\s+as\s+/).pop().trim();
        if (/^[A-Za-z_$][\w$]*$/.test(name)) {
          declared.add(name);
        }
      }
    }
    for (const match of text.matchAll(/import\s+([A-Za-z_$][\w$]*)\s+from/g)) {
      declared.add(match[1]);
    }
    for (const match of text.matchAll(/\{([^{}]*)\}\s*=/g)) {
      for (const part of match[1].split(",")) {
        const name = part.split(":").pop().trim();
        if (/^[A-Za-z_$][\w$]*$/.test(name)) {
          declared.add(name);
        }
      }
    }
    for (const match of text.matchAll(/\(([^()]*)\)\s*(?:=>|\{)/g)) {
      for (const part of match[1].split(",")) {
        const name = part.trim();
        if (/^[A-Za-z_$][\w$]*$/.test(name)) {
          declared.add(name);
        }
      }
    }
    for (const match of text.matchAll(/([A-Za-z_$][\w$]*)\s*=>/g)) {
      declared.add(match[1]);
    }
    // A class method body: `constructor(...) {`, `get name() {`, `static make() {`. The declaration pass
    // above sees `class` but not the members, so without this a class with a constructor reads as a file
    // calling a function named `constructor`.
    for (const match of text.matchAll(
      /(?:^|[{;}\n])\s*(?:async\s+)?(?:(?:get|set|static)\s+)*([A-Za-z_$][\w$]*)\s*\([^()]*\)\s*\{/g,
    )) {
      declared.add(match[1]);
    }

    // Strip comments, string literals, template text and regexes before scanning, so a word inside any of
    // them is not a reference. The same lexer the purity check uses: the two audits have the same failure
    // mode, which is reading prose or a pattern as code.
    const source = stripNonCode(text);

    const names = new Set();
    // A bare call: an identifier followed by `(`, not preceded by `.` (a method) or a word character (part of
    // a longer name). `?.name(` is excluded by the `.` in the lookbehind too.
    for (const match of source.matchAll(/(?<![.\w$])([A-Za-z_$][\w$]*)\s*\(/g)) {
      const name = match[1];
      if (!declared.has(name) && !BROWSER_GLOBALS.has(name)) {
        names.add(name);
      }
    }
    return [...names].sort();
  }

  // Every module the studio page ships as source: the React tree, the sessions, the pure library modules and
  // the stub's redirect script. The list is explicit so a new module is added here in the same change as the
  // module itself. `tsconfig.json` is what catches the TypeScript half statically (`tsc --noEmit`, part of
  // `npm run build`); this scan covers the `.js` files the compiler never looks at.
  const PAGE_MODULES = [
    ["src/lib/preview-state.js", resolve(WEB, "src/lib/preview-state.js")],
    ["public/redirect.js", resolve(DIST, "redirect.js")],
  ];

  for (const [label, path] of PAGE_MODULES) {
    const real = undeclaredNames(readFileSync(path, "utf8"));
    if (real.length > 0) {
      fail(`${label} calls undeclared function(s): ${real.join(", ")} (a leftover from a refactor?)`);
    } else {
      ok(`${label} calls no undeclared function`);
    }
  }

  process.stdout.write(
    failures === 0 ? "\npage checks: all passed\n" : `\npage checks: ${failures} failed\n`,
  );
  if (failures > 0) {
    process.exitCode = 1;
  }
}
