#!/usr/bin/env node
/**
 * The live edit's acceptance probe (issue #444): type in the editor and check what the page does,
 * with no click anywhere.
 *
 * **Why this is a separate script rather than more assertions in `preview_browser_verify.mjs`.** That
 * script answers "does the page work at all"; this one answers the six sentences of the issue's
 * acceptance, each of which is about *what a keystroke costs*: a frame count, a state cell, a
 * surviving picture. They need a different shape of test -- typing, waiting for the debounce to
 * settle, reading the module's own counters before and after -- and mixing them into the smoke pass
 * would make both scripts harder to read and the failure messages less specific.
 *
 * It is **developer-run** for the same reason the other browser scripts are: it needs a browser with
 * WebGPU, which no CI lane has (an owner decision, the same one P3's pixel parity waits on), and it
 * is not a pixel golden.
 *
 * The six clauses, in the issue's order:
 *
 *   1. Typing in the editor updates the canvas and the scene with no click.
 *   2. A state cell (the Counter's `count`) survives a text edit that keeps its component and cell id.
 *   3. A syntax error leaves the previous frame up and shows a diagnostic.
 *   4. **Restart app** resets the count to 0.
 *   5. A static scene draws zero frames after the edit settles (render on demand).
 *   6. A burst of keystrokes makes exactly one frame (the debounce and `requestFrame` compose).
 *
 * Usage:
 *   node scripts/ci/preview_browser_live_edit.mjs --site build/pages [--shot out.png]
 *
 * The site must be the assembled artifact (see `.github/workflows/pages.yml`'s assemble step): the
 * built front end plus the three Wasm modules. Served over HTTP because a Wasm module cannot be
 * fetched from `file://`.
 */

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { ensureProject, openPanel, tab } from './studio_project.mjs';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.ttf': 'font/ttf',
  '.txt': 'text/plain; charset=utf-8',
  '.tsx': 'text/plain; charset=utf-8',
  '.svg': 'image/svg+xml',
};

function argument(name, fallback) {
  const index = process.argv.indexOf(name);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
}

const site = argument('--site', 'build/pages');
const shot = argument('--shot', null);

let chromium;
try {
  ({ chromium } = await import('playwright'));
} catch {
  console.error(
    'playwright is not installed. This script is developer-run on purpose (no repository dependency):\n' +
      '  npm install playwright && npx playwright install chromium\n' +
      'Run it from a directory whose node_modules has playwright, or install it globally.',
  );
  process.exit(2);
}

const server = createServer(async (request, response) => {
  const url = new URL(request.url, 'http://127.0.0.1');
  let path = normalize(decodeURIComponent(url.pathname));
  if (path === '/' || path === '\\') path = '/index.html';
  try {
    const body = await readFile(join(site, path));
    response.writeHead(200, {
      'content-type': MIME[extname(path).toLowerCase()] ?? 'application/octet-stream',
      'cache-control': 'no-store',
    });
    response.end(body);
  } catch {
    response.writeHead(404, { 'content-type': 'text/plain' });
    response.end('not found');
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;
console.log(`serving ${site} at ${base}`);

const browser = await chromium.launch({
  args: [
    // A software WebGPU adapter is enough to prove acquisition, handover and draw; it is not a golden.
    '--enable-unsafe-webgpu',
    '--enable-features=Vulkan',
    '--use-angle=swiftshader',
    '--disable-vulkan-surface',
  ],
});
const page = await (await browser.newContext({ viewport: { width: 1600, height: 1000 } })).newPage();

const consoleErrors = [];
page.on('console', (message) => {
  if (message.type() === 'error') consoleErrors.push(message.text());
});
page.on('pageerror', (error) => consoleErrors.push(`pageerror: ${String(error)}`));

let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok   ' : 'FAIL '} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
};

// --- helpers ---------------------------------------------------------------------------------------

const text = async (selector) =>
  page.evaluate((id) => document.querySelector(id)?.textContent?.trim() ?? null, selector);

/** The canvas pane's own frame counter: `#stat-frame` reads "<w>x<h> (#N)". */
const frames = async () => {
  const raw = await text('#stat-frame');
  const match = /#(\d+)/.exec(raw ?? '');
  return match ? Number(match[1]) : null;
};

/**
 * The running evaluation's first state cell's value, from the canvas pane's state row.
 *
 * The row prints `name=value` per cell and mirrors a single scalar cell on `#stat-state`'s
 * `data-value`, which is what this reads: the Counter's `count` is exactly that case.
 */
const stateValue = async () => {
  const raw = await page.evaluate(() => {
    const node = document.getElementById('stat-state');
    if (!node || node.hidden) return null;
    return node.getAttribute('data-value');
  });
  return raw === null ? null : Number(raw);
};

/** Writes the whole editor buffer as one input event, the way a paste does. */
const setSource = async (source) => {
  await page.evaluate((next) => {
    window.__tesseraEditor.edit(next);
  }, source);
};

/** Types one character at a time, like a person: one `input` event per keystroke. */
const typeText = async (characters) => {
  for (const character of characters) {
    await page.evaluate((one) => {
      const editor = window.__tesseraEditor;
      editor.edit(editor.text() + one);
    }, character);
    // Deliberately shorter than the debounce, so this really is a burst: if the page applied per
    // keystroke, each of these would land inside the previous one's window.
    await page.waitForTimeout(20);
  }
};

/** Waits for the debounce (150 ms) plus the apply and a frame, with margin. */
const settle = (extra = 900) => page.waitForTimeout(extra);

const openCanvas = async () => {
  await tab(page, 'canvas');
  await page.waitForFunction(
    () => {
      const status = document.querySelector('#canvas-status')?.textContent ?? '';
      return (
        !document.querySelector('#canvas-frame')?.hidden ||
        !document.querySelector('#unsupported')?.hidden ||
        /fail|drew|no draw/.test(status)
      );
    },
    undefined,
    { timeout: 20000 },
  );
};

const openScene = async () => {
  await tab(page, 'scene');
  await page.waitForFunction(
    () => (document.querySelectorAll('#nodes .node').length ?? 0) > 0,
    undefined,
    { timeout: 20000 },
  );
};

// --- the run ---------------------------------------------------------------------------------------

await page.goto(`${base}/index.html`, { waitUntil: 'load' });

// No project boots by default (issue #391), so open one before anything is expected to compile (#849).
await ensureProject(page);

// The browser under test really has WebGPU, otherwise clauses 1, 3, 5 and 6 are meaningless.
const hasGpu = await page.evaluate(async () => {
  if (!navigator.gpu) return false;
  const adapter = await navigator.gpu.requestAdapter();
  return adapter ? Boolean(await adapter.requestDevice()) : false;
});
check('clause 0: WebGPU device is available', hasGpu);
if (!hasGpu) {
  await browser.close();
  server.close();
  process.exit(1);
}

// The first automatic apply: the page must not need a click to show anything (issue #444).
// Diagnostics is not in the default layout any more - its group starts collapsed (issue #431) - so the
// panel area is opened first, which is what makes `#diag-status` exist to read (#849).
await openPanel(page, 'diagnostics');
await page.waitForFunction(
  () => {
    const status = document.querySelector('#diag-status')?.textContent ?? '';
    return status !== '' && status !== 'ready' && status !== 'not run yet';
  },
  undefined,
  { timeout: 20000 },
);
check('first load applies itself (no "press Run" empty state)', true, await text('#diag-status'));

await openCanvas();
const firstFrames = await frames();
check('clause 1a: the canvas drawn on its first open, with no click', firstFrames !== null && firstFrames >= 1);

// --- clause 5: a static scene draws zero frames after the edit settles ------------------------------
//
// The editor's text is replaced with a document that differs only in a string, so the apply is a real
// re-evaluation, and then the page is left alone. A render-on-demand page draws once for the edit and
// nothing afterwards; a frame loop would keep the counter climbing.
const counterSource = `export function Preview(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Text>Count: {count}</Text>
      <Button automationId="7" onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;
await setSource(counterSource);
await settle(1400);
const afterEdit = await frames();
await page.waitForTimeout(1500);
const afterIdle = await frames();
check(
  'clause 5: a static scene draws zero frames after the edit settles',
  afterEdit !== null && afterEdit === afterIdle,
  `${afterEdit} -> ${afterIdle}`,
);

// --- clause 1b: the scene pane follows the same edit, with no click ---------------------------------
//
// The edit changes a *literal* Text child, because that is what the snapshot prints: a `{count}` child
// is dynamic and its evaluated text is not part of the canonical node document, so it could not prove
// the scene pane saw the new source.
await openScene();
const nodesFor = async () => page.locator('#nodes .node').count();
const nodesBefore = await nodesFor();
await tab(page, 'canvas');
await settle(300);
await setSource(
  counterSource
    .replace('Count: {count}', 'SceneMarker: the scene pane followed')
    .replace('const [count, setCount] = useState<int>(0);', 'const [count, setCount] = useState<int>(0);'),
);
await settle(1400);
await openScene();
const nodesAfter = await nodesFor();
check(
  'clause 1b: the scene pane updated from the same edit, with no click',
  nodesAfter > 0,
  `${nodesBefore} -> ${nodesAfter} node(s)`,
);
const sceneText = await page.evaluate(() =>
  [...document.querySelectorAll('#nodes .node')].map((row) => row.textContent).join(' '),
);
check(
  'clause 1b: the scene shows the edited text',
  /SceneMarker/.test(sceneText),
  sceneText.slice(0, 120),
);

// --- clause 2: a state cell survives a text edit ---------------------------------------------------
//
// **How the count gets a value the edit can carry.** Canvas pointer input is #388's page half, which is
// a different slice and deliberately not wired here, so this does not click the canvas. It uses the
// carrier's own rule instead, which is a sharper test of it: source A declares `useState<int>(5)`, so
// the running evaluation's cell is 5; the edit then changes the initializer to `0` *and* the text. A
// carry must keep **5**, because the cell's identity (component id, cell id, IR type) still matches;
// a reset would show 0. That separates "carried" from "re-initialized" without needing a pointer.
//
// The canvas tab must be open for this: a hidden canvas session does not paint (render on demand), so
// it does not evaluate either, and its state row would be describing the last time it was visible.
const withInitial = (initial, text) => `export function Preview(): Node {
  const [count, setCount] = useState<int>(${initial});
  return (
    <View>
      <Text>${text}</Text>
      <Button automationId="7" onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;
await tab(page, 'canvas');
await settle(900);
// The `5` document is written first, and **Restart app** then cold-starts *it*, so the 5 is that
// document's initializer. The order matters: restarting before writing the source would cold-start the
// previous document, whose cell this one would then carry (which is the rule working, not a test).
await setSource(withInitial(5, 'Count: {count}'));
await settle(1600);
await page.locator('#restart-app').click();
await settle(1800);
const countAfterFirstApply = await stateValue();
const framesBeforeCarry = await frames();
check(
  'clause 2: a fresh document starts at its initializer',
  countAfterFirstApply === 5,
  `count ${countAfterFirstApply}`,
);
// The edit changes both the initializer and the text, so a re-evaluation definitely happens: only the
// carry rule can make the count still read 5.
await setSource(withInitial(0, 'Count: {count}, edited'));
await settle(1800);
const countAfterEdit = await stateValue();
const framesAfterCarry = await frames();
check(
  'clause 2: the edit applied with exactly one frame',
  framesBeforeCarry !== null && framesAfterCarry === framesBeforeCarry + 1,
  `frames ${framesBeforeCarry} -> ${framesAfterCarry}`,
);
check(
  'clause 2: a state cell survives a text edit that keeps its component and cell id',
  countAfterFirstApply === 5 && countAfterEdit === 5,
  `count ${countAfterFirstApply} -> ${countAfterEdit} (the new initializer is 0, so 5 is carried)`,
);
check(
  'clause 2: the state row names the cell the carry matched on',
  (await page.evaluate(() => document.getElementById('stat-state')?.textContent ?? '')).includes('count'),
  await page.evaluate(() => document.getElementById('stat-state')?.textContent ?? ''),
);

// --- clause 3: a syntax error keeps the previous frame and shows a diagnostic -----------------------
const beforeError = await frames();
const beforeErrorValue = await stateValue();
await setSource(withInitial(0, 'Count: {count}, edited').replace('</View>', '</Vie'));
await settle(1600);
const afterErrorFrames = await frames();
const afterErrorValue = await stateValue();
const canvasStatusAfterError = await text('#canvas-status');
const failedShown = await page.evaluate(() => !document.getElementById('failed')?.hidden);
check(
  'clause 3: a syntax error draws no frame and keeps the last good picture',
  afterErrorFrames === beforeError,
  `${beforeError} -> ${afterErrorFrames}`,
);
check(
  'clause 3: the canvas is not blanked by the error',
  !failedShown && !/unsupported/i.test(canvasStatusAfterError ?? ''),
  `status "${canvasStatusAfterError}", failed box ${failedShown ? 'shown' : 'hidden'}`,
);
check(
  'clause 3: the state the error did not reach is still the one on screen',
  afterErrorValue === beforeErrorValue,
  `${beforeErrorValue} -> ${afterErrorValue}`,
);
await openPanel(page, 'diagnostics');
await tab(page, 'diagnostics');
await settle(1600);
const diagnosticsText = await page.evaluate(
  () =>
    (document.getElementById('diagnostics')?.textContent ?? '') +
    ' | ' +
    (document.getElementById('diag-status')?.textContent ?? ''),
);
check(
  'clause 3: the diagnostic is shown for the failing edit',
  diagnosticsText.trim() !== '' && !/no diagnostics/.test(diagnosticsText) && !/^\s*\|\s*ready\s*$/.test(diagnosticsText),
  diagnosticsText.slice(0, 200),
);

// Recover to a valid document that still carries 5, so clause 4 has a non-zero count to reset.
await tab(page, 'canvas');
await settle(900);
await setSource(withInitial(0, 'Count: {count}, recovered'));
await settle(1800);
// A carried 5 here, not a reset 0: the failure documents above did not touch the running evaluation.
const countBeforeRestart = await stateValue();
check(
  'clause 2b: the count carried across the error and the recovery',
  countBeforeRestart === 5,
  `count ${countBeforeRestart}`,
);

// --- clause 4: Restart app resets the count --------------------------------------------------------
// (The count is read on the canvas tab, where the session that owns it is the one running.)
check(
  'clause 4: the count is non-zero before the restart (otherwise the reset proves nothing)',
  countBeforeRestart !== null && countBeforeRestart !== 0,
  `count ${countBeforeRestart}`,
);
check(
  'clause 4: Restart app is the button, name and id',
  (await page.locator('#restart-app').textContent())?.trim() === 'Restart app',
);
await page.locator('#restart-app').click();
await settle(2000);
const countAfterRestart = await stateValue();
check(
  'clause 4: Restart app resets the count',
  countAfterRestart !== null && countAfterRestart === 0,
  `${countBeforeRestart} -> ${countAfterRestart}`,
);

// --- clause 6: a burst of keystrokes makes exactly one frame ---------------------------------------
await tab(page, 'canvas');
await settle(600);
const beforeBurst = await frames();
// Six keystrokes 20 ms apart: all six land inside one 150 ms window, so the page must apply once.
await typeText('      ');
await settle(1600);
const afterBurst = await frames();
check(
  'clause 6: a burst of keystrokes makes one frame',
  beforeBurst !== null && afterBurst !== null && afterBurst - beforeBurst === 1,
  `${beforeBurst} -> ${afterBurst}`,
);

// --- the sorry state of a page that drew nothing ---------------------------------------------------
check('no console errors', consoleErrors.length === 0, consoleErrors.slice(0, 3).join(' | ') || 'none');

if (shot) {
  await tab(page, 'canvas');
  await settle(500);
  await page.screenshot({ path: shot });
  console.log(`screenshot: ${shot}`);
}

await browser.close();
server.close();

console.log(failures === 0 ? '\nlive edit: all clauses passed' : `\nlive edit: ${failures} failure(s)`);
process.exit(failures === 0 ? 0 : 1);
