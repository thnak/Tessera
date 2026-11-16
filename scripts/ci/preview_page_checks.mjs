// SPDX-License-Identifier: MIT
//
// Static checks for the studio page's wiring (issue #392, after the three-page merge; the checks
// started as issue #169's P5 wiring checks).
//
//   node scripts/ci/preview_page_checks.mjs
//
// Five checks, each for a failure this work actually hit or would have shipped:
//
// 1. **Every element `studio.js` looks up exists in the page.** `mountStudio` resolves its nodes by
//    id, so a page that drops one throws at load -- invisible to a stub, and invisible to `node
//    --check`.
// 2. **Every id a view module resolves exists in the page.** The three views each hold their own DOM
//    contract (`view-diagnostics.js`, `view-scene.js`, `view-canvas.js`), and the merge is exactly the
//    kind of change that moves a pane and loses an id. The list is per view, so a failure names the
//    view whose pane is wrong rather than "the page".
// 3. **The redirect stubs carry the query and the fragment.** This is the share-link migration: the
//    old URLs must forward to `index.html` with `location.search` and `location.hash` intact and the
//    view selected. The check drives the real `redirect.js` under Node -- with no browser -- against
//    the four old-URL shapes a real link takes, including the empty one.
// 4. **Every `samples.json` path exists.** The Pages lane copies `samples/` into the site and the
//    gallery fetches `./<path>`; a missing file is a button that cannot load. This passes locally
//    (where `samples/` sits beside `designer/`) and fails only on the deployed site, which is the
//    worst place to find it.
// 5. **No page calls a function it does not declare.** `app.js` kept a `void start()` after `start()`
//    was folded into `mountStudio`, and `scene.js` used a `file` its refactor had deleted; both throw
//    only when that line runs. The audit is deliberately shallow -- a scope-free identifier scan, not
//    a parser -- because it only has to catch "the name is gone".
//
// This is NOT a rendering test. That the page paints is what the deploy and a human opening the URL
// verify; nothing here claims otherwise.

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { pathToFileURL } from 'node:url';

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const WEB = resolve(REPO_ROOT, 'designer', 'web');

// The ids `studio.js` (`mountStudio`) looks up. One page now, so one list, checked against one file.
const SHARED_IDS = [
  'controls',
  'share',
  'share-status',
  'toggle-gallery',
  'gallery',
  'gallery-list',
  'link-note',
  'source',
];

// The page shell's own ids: the entry module (`studio-page.js`) and the view switcher (`shell.js`)
// resolve these, and nothing else would catch one being renamed out from under them.
const SHELL_IDS = [
  'tab-diagnostics',
  'tab-scene',
  'tab-canvas',
  'view-diagnostics',
  'view-scene',
  'view-canvas',
  'run',
  'load-sample',
  'build-id',
  'page-hint',
  // The hint under the shared editor. The page owns it (`shell.js` writes the per-view sentence), so
  // it is a shell id rather than any one view's.
  'editor-hint',
];

// Each view's own ids, kept separate so a failure names the view whose pane is wrong.
const VIEW_IDS = {
  diagnostics: ['diag-status', 'diagnostics', 'diag-empty'],
  scene: [
    'scene-status',
    'nodes',
    'scene-empty',
    'scene-hint',
    'script',
    'run-input',
    'input-status',
    'cells',
  ],
  canvas: [
    'canvas-status',
    'unsupported',
    'unsupported-heading',
    'unsupported-body',
    'canvas-frame',
    'canvas',
    'failed',
    'failed-body',
    'frame-stats',
    'stat-frame',
    'stat-draws',
    'stat-vertices',
    'stat-nodes',
    'stat-dpr',
  ],
};

// Browser and language globals a page may use without declaring. Kept explicit so the audit fails on
// an unknown name instead of silently accepting one.
const BROWSER_GLOBALS = new Set([
  'document', 'window', 'globalThis', 'console', 'fetch', 'navigator', 'location', 'history',
  'localStorage', 'JSON', 'Math', 'Number', 'String', 'Boolean', 'Array', 'Object', 'Promise',
  'Error', 'TypeError', 'Set', 'Map', 'Date', 'TextEncoder', 'TextDecoder', 'Blob', 'Response',
  'Uint8Array', 'CompressionStream', 'DecompressionStream', 'getComputedStyle', 'parseFloat',
  'parseInt', 'isNaN', 'Reflect', 'Proxy', 'undefined', 'null', 'true', 'false', 'this',
  'arguments', 'await', 'async', 'function', 'return', 'if', 'else', 'for', 'while', 'const', 'let',
  'var', 'new', 'typeof', 'instanceof', 'of', 'in', 'do', 'switch', 'case', 'break', 'continue',
  'try', 'catch', 'finally', 'throw', 'class', 'extends', 'super', 'import', 'export', 'default',
  'from', 'void', 'delete', 'yield', 'static', 'get', 'set', 'encodeURIComponent', 'decodeURIComponent',
  'URLSearchParams',
]);

// Callback parameters a shallow scan cannot bind. `d` is the diagnostics view's `filter((d) => ...)`.
const KNOWN_CALLBACK_PARAMS = new Set(['d']);

let failures = 0;

function fail(message) {
  process.stdout.write(`FAIL ${message}\n`);
  failures += 1;
}

function ok(message) {
  process.stdout.write(`ok   ${message}\n`);
}

function html() {
  return readFileSync(resolve(WEB, 'index.html'), 'utf8');
}

// --- 1. the page carries the elements the shared chrome looks up --------------------------------

{
  const page = html();
  let missing = 0;
  for (const id of [...SHARED_IDS, ...SHELL_IDS]) {
    if (!page.includes(`id="${id}"`)) {
      fail(`index.html is missing id="${id}" that studio.js/studio-page.js/shell.js require`);
      missing += 1;
    }
  }
  if (missing === 0) {
    ok(`index.html carries every element the shared chrome and the shell look up (${SHARED_IDS.length + SHELL_IDS.length})`);
  }
}

// --- 2. each view's pane carries the ids that view resolves --------------------------------------

for (const [view, ids] of Object.entries(VIEW_IDS)) {
  const page = html();
  let missing = 0;
  for (const id of ids) {
    if (!page.includes(`id="${id}"`)) {
      fail(`index.html is missing id="${id}" that the ${view} view resolves`);
      missing += 1;
    }
  }
  if (missing === 0) {
    ok(`the ${view} pane carries every id its view resolves (${ids.length})`);
  }
}

// --- 3. the redirect stubs preserve the query, the fragment and the view -------------------------

// The real module, loaded the way a browser loads it: it is a classic script that publishes
// `globalThis.tesseraRedirect`, so importing it under Node is the same code path with a fake global.
const REDIRECT_URL = pathToFileURL(resolve(WEB, 'redirect.js')).href;
await import(REDIRECT_URL);

const redirect = globalThis.tesseraRedirect;
if (!redirect || typeof redirect.mergedUrl !== 'function') {
  fail('redirect.js does not publish tesseraRedirect.mergedUrl (the stubs would not forward)');
} else {
  // The old-URL shapes a real link takes, including the ones with nothing in them. A share link's
  // fragment is the whole project state, so it is compared byte for byte rather than "present".
  const PAYLOAD = 'zKLUv_QBFYgA'; // a real-looking deflate-raw payload, short enough to read
  const cases = [
    {
      why: 'a pre-merge share link with a query and a fragment',
      view: 'scene',
      search: '?viewport=390x844&dpr=2&theme=dark',
      hash: '#p5=' + PAYLOAD,
      want:
        'index.html?viewport=390x844&dpr=2&theme=dark&view=scene#p5=' + PAYLOAD,
    },
    {
      why: 'a pre-merge link with no query and no fragment',
      view: 'canvas',
      search: '',
      hash: '',
      want: 'index.html?view=canvas',
    },
    {
      why: 'a pre-merge link with a fragment only',
      view: 'canvas',
      search: '',
      hash: '#p5=' + PAYLOAD,
      want: 'index.html?view=canvas#p5=' + PAYLOAD,
    },
    {
      why: 'a pre-merge link with a query only',
      view: 'scene',
      search: '?theme=light',
      hash: '',
      want: 'index.html?theme=light&view=scene',
    },
    {
      why: 'a stale ?view= in the old query is replaced, not duplicated',
      view: 'canvas',
      search: '?view=scene&dpr=3',
      hash: '#p5=abc',
      want: 'index.html?view=canvas&dpr=3#p5=abc',
    },
    {
      why: 'a fragment that is not a share payload is still carried verbatim',
      view: 'scene',
      search: '',
      hash: '#section-2',
      want: 'index.html?view=scene#section-2',
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
    ['scene.html', 'scene'],
    ['canvas.html', 'canvas'],
  ]) {
    const stub = readFileSync(resolve(WEB, page), 'utf8');
    const problems = [];
    if (!stub.includes('src="redirect.js"')) {
      problems.push('does not load redirect.js');
    }
    if (!stub.includes(`redirectTo('${view}')`)) {
      problems.push(`does not forward to the ${view} view`);
    }
    if (/type="module"|tessera_(diagnostics|scene_preview|canvas)\.js/.test(stub)) {
      problems.push('loads engine code (a stub must not depend on a module loading)');
    }
    if (problems.length > 0) {
      fail(`${page} ${problems.join('; ')}`);
    } else {
      ok(`${page} is a redirect stub to the ${view} view with no engine code`);
    }
  }
}

// --- 4. every manifest path exists, under the path the site uses ---------------------------------

const manifest = JSON.parse(readFileSync(resolve(WEB, 'samples.json'), 'utf8'));
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

// --- 5. no page module calls a function it does not declare --------------------------------------

/**
 * Names this file *calls* as a bare function but neither declares, imports, nor inherits.
 *
 * The scan is deliberately shallow -- a scope-free identifier scan, not a parser -- because it only
 * has to catch "the name is gone". It is also deliberately precise about what counts as a call: the
 * candidate must be an identifier immediately followed by `(` and *not* preceded by `.` (so a method
 * call on an object is not a free identifier). That precision is what keeps an object literal's key
 * (`load: loadModule`) and a regex flag (`/\+/g`) from being reported as dangling calls, which is the
 * failure mode a stricter-looking-but-sloppier scan has: it flags `key: value` because the same name
 * also appears as `obj.key()`, and the check becomes noise nobody reads.
 */
function undeclaredNames(text) {
  const declared = new Set();
  for (const match of text.matchAll(/\b(?:function|const|let|var|class)\s+([A-Za-z_$][\w$]*)/g)) {
    declared.add(match[1]);
  }
  for (const match of text.matchAll(/import\s*\{([^}]*)\}/g)) {
    for (const part of match[1].split(',')) {
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
    for (const part of match[1].split(',')) {
      const name = part.split(':').pop().trim();
      if (/^[A-Za-z_$][\w$]*$/.test(name)) {
        declared.add(name);
      }
    }
  }
  for (const match of text.matchAll(/\(([^()]*)\)\s*(?:=>|\{)/g)) {
    for (const part of match[1].split(',')) {
      const name = part.trim();
      if (/^[A-Za-z_$][\w$]*$/.test(name)) {
        declared.add(name);
      }
    }
  }
  for (const match of text.matchAll(/([A-Za-z_$][\w$]*)\s*=>/g)) {
    declared.add(match[1]);
  }

  // Strip comments and string literals before scanning, so a word inside either is not a reference.
  const source = text
    .replace(/\/\/[^\n]*/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/`(?:\\.|[^`\\])*`/g, '""')
    .replace(/'(?:\\.|[^'\\])*'/g, '""')
    .replace(/"(?:\\.|[^"\\])*"/g, '""');

  const names = new Set();
  // A bare call: an identifier followed by `(`, not preceded by `.` (a method) or a word character
  // (part of a longer name). `?.name(` is excluded by the `.` in the lookbehind too.
  for (const match of source.matchAll(/(?<![.\w$])([A-Za-z_$][\w$]*)\s*\(/g)) {
    const name = match[1];
    if (!declared.has(name) && !BROWSER_GLOBALS.has(name)) {
      names.add(name);
    }
  }
  return [...names].sort();
}

// Every module the merged page ships, plus the stub's redirect script. The list is explicit so a new
// module is added here in the same change as the module itself.
const PAGE_MODULES = [
  'studio-page.js',
  'shell.js',
  'view-diagnostics.js',
  'diagnostics-view-render.js',
  'view-scene.js',
  'scene-view-render.js',
  'view-canvas.js',
  'canvas-device.js',
  'canvas-pane.js',
  'wasm-call.js',
  'scene-input.js',
  'redirect.js',
];

for (const page of PAGE_MODULES) {
  const text = readFileSync(resolve(WEB, page), 'utf8');
  const real = undeclaredNames(text).filter((name) => !KNOWN_CALLBACK_PARAMS.has(name));
  if (real.length > 0) {
    fail(`${page} calls undeclared function(s): ${real.join(', ')} (a leftover from a refactor?)`);
  } else {
    ok(`${page} calls no undeclared function`);
  }
}

process.stdout.write(
  failures === 0 ? '\npage checks: all passed\n' : `\npage checks: ${failures} failed\n`,
);
if (failures > 0) {
  process.exitCode = 1;
}
