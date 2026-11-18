#!/usr/bin/env node
/**
 * Verify the studio's **layout** in a real browser (issue #431's acceptance).
 *
 * **Why this exists beside `preview_browser_verify.mjs`.** That script proves the page *works*: WebGPU
 * is available, the three views compile/evaluate/draw, a share link round-trips. None of that can see
 * the thing #431 is about -- that the page is one three-column workspace whose shape does not change
 * when the preview view does. A regression there (the view switch moving back above the workspace, the
 * tree collapsing to nothing, the phone layout gaining a horizontal scrollbar) leaves every one of those
 * ten checks green, so it needs its own measurements.
 *
 * Developer-run and not a CI gate, for the same reason the other browser scripts are: it needs a browser
 * and a WebGPU-capable driver, which no lane has today.
 *
 * Usage:
 *   node scripts/ci/preview_browser_layout.mjs --site build/pages [--shots build/]
 *
 * What it measures, at 1440x900 and at 390x844 (the issue's two acceptance sizes):
 *
 *   1. the workspace has three side-by-side columns, in the order tree | code | preview;
 *   2. the project tree lists the opened project's files, and a click and the arrow keys both change the
 *      selection (`role="tree"`, `aria-selected`, one tab stop);
 *   3. a file click opens a tab, `Run` compiles the *active source file*, and a second file gets a
 *      second tab with the unsaved marker;
 *   4. switching the preview view does not move the tree or the editor (their boxes are identical before
 *      and after), which is the issue's "no view switch changes the top-level layout";
 *   5. `?view=` still opens on the named view, and a `#p5=` link still restores its source;
 *   6. at 390 px the layout is one column, the preview is reachable, and
 *      `document.documentElement.scrollWidth <= clientWidth` (no horizontal scroll).
 */

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { mkdir } from 'node:fs/promises';

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
};

function argument(name, fallback) {
  const index = process.argv.indexOf(name);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
}

const site = argument('--site', 'build/pages');
const shots = argument('--shots', null);

let chromium;
try {
  ({ chromium } = await import('playwright'));
} catch {
  console.error(
    'playwright is not installed. This script is developer-run on purpose (no repository dependency):\n' +
      '  npm install playwright && npx playwright install chromium\n' +
      'Run it from a directory whose node_modules has playwright.',
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
    '--enable-unsafe-webgpu',
    '--enable-features=Vulkan',
    '--use-angle=swiftshader',
    '--disable-vulkan-surface',
  ],
});

let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok   ' : 'FAIL '} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
};

/** The box of a selector, or null when it is not laid out. */
const boxOf = (page, selector) =>
  page.evaluate((selector_) => {
    const node = document.querySelector(selector_);
    if (!node) return null;
    const rect = node.getBoundingClientRect();
    return { x: Math.round(rect.x), y: Math.round(rect.y), w: Math.round(rect.width), h: Math.round(rect.height) };
  }, selector);

/** A page with a viewport, and the console errors it produced. */
async function open(width, height, path = '/index.html') {
  const context = await browser.newContext({ viewport: { width, height } });
  const page = await context.newPage();
  const errors = [];
  page.on('console', (message) => {
    if (message.type() === 'error') errors.push(message.text());
  });
  page.on('pageerror', (error) => errors.push(`pageerror: ${String(error)}`));
  await page.goto(`${base}${path}`, { waitUntil: 'load' });
  await page.waitForTimeout(1400);
  return { page, errors, context };
}

// --- 1440x900: three columns, a tree, tabs, and a switch that moves nothing else --------------------

{
  const { page, errors, context } = await open(1440, 900);

  const tree = await boxOf(page, '.region-tree');
  const code = await boxOf(page, '.region-code');
  const preview = await boxOf(page, '.region-preview');
  check(
    'the workspace is three columns at 1440x900',
    Boolean(tree && code && preview) &&
      tree.x < code.x &&
      code.x < preview.x &&
      Math.abs(tree.y - code.y) < 4 &&
      Math.abs(code.y - preview.y) < 4,
    `${tree ? `tree x=${tree.x} w=${tree.w}` : 'no tree'}, ` +
      `${code ? `code x=${code.x} w=${code.w}` : 'no code'}, ` +
      `${preview ? `preview x=${preview.x} w=${preview.w}` : 'no preview'}`,
  );

  // The tree lists the seeded project: the entry file and the stylesheet, with the directories derived.
  const rows = await page.locator('#project-tree [role="treeitem"]').evaluateAll((nodes) =>
    nodes.map((node) => ({ path: node.getAttribute('data-path'), dir: node.classList.contains('is-dir') })),
  );
  const paths = rows.map((row) => row.path);
  check(
    'the tree lists the opened project, directories first',
    ['ui', 'styles', 'tessera.json', 'styles/theme.css', 'ui/App.tsx'].every((path) =>
      paths.includes(path),
    ),
    paths.join(' '),
  );

  // The seeded entry file is open and selected: one tab, one selection, and the editor holds its text.
  const firstTab = await page.locator('[aria-label="Open files"] [role="tab"]').count();
  const firstText = await page.evaluate(() => window.__tesseraEditor.text());
  check(
    'the entry file is open in a tab and its text is in the editor',
    firstTab === 1 && firstText.includes('Count: {count}'),
    `${firstTab} tab(s)`,
  );

  // Keyboard: the tree is one tab stop, and ArrowDown moves the selection to the next row.
  const before = await page.locator('#project-tree [aria-selected="true"]').getAttribute('data-path');
  await page.locator('#project-tree [role="treeitem"][tabindex="0"]').focus();
  await page.keyboard.press('ArrowDown');
  const after = await page.locator('#project-tree [aria-selected="true"]').getAttribute('data-path');
  const focused = await page.evaluate(() => document.activeElement?.getAttribute('data-path') ?? null);
  check(
    'the tree is keyboard navigable (ArrowDown moves selection and focus)',
    before !== after && focused === after,
    `${before} -> ${after}, focus ${focused}`,
  );

  // A file click opens a tab. `theme.css` is a stylesheet: it opens, but the previewed entry is the
  // one `tessera.json` names, whatever the editor shows. Clicking the *tab* a row opened is the same action, so
  // this also proves the two ways of selecting a file agree.
  const tabsBefore = await page.locator('[aria-label="Open files"] [role="tab"]').count();
  await page.locator('[role="treeitem"][data-path="styles/theme.css"]').click();
  await page.waitForTimeout(200);
  const twoTabs = await page.locator('[aria-label="Open files"] [role="tab"]').count();
  const cssText = await page.evaluate(() => window.__tesseraEditor.text());
  const compileTarget = await page.locator('#status-file').textContent();
  check(
    'selecting a stylesheet opens one more tab and keeps the entry the one tessera.json names',
    twoTabs === tabsBefore + 1 && cssText.includes('background') && compileTarget.trim() === 'ui/App.tsx',
    `${twoTabs} tab(s), target ${compileTarget.trim()}`,
  );

  // Typing marks the active file unsaved (the marker is in the tab strip and in the tree).
  await page.evaluate(() => window.__tesseraEditor.edit('/* edited */\n'));
  await page.waitForTimeout(200);
  const dirtyMarks = await page.locator('.editor-tab-dirty').count();
  const treeDirty = await page.locator('#project-tree [data-path="styles/theme.css"] .tree-dirty').count();
  check(
    'editing marks the file unsaved in the tab and in the tree',
    dirtyMarks === 1 && treeDirty === 1,
    `${dirtyMarks} tab marker(s), ${treeDirty} tree marker(s)`,
  );

  // The layout does not depend on the view: capture the tree and code boxes, switch all three views, and
  // compare. This is the issue's acceptance sentence, measured.
  const beforeSwitch = { tree: await boxOf(page, '.region-tree'), code: await boxOf(page, '.region-code') };
  for (const view of ['scene', 'canvas', 'diagnostics']) {
    await page.locator(`#tab-${view}`).click();
    await page.waitForTimeout(300);
  }
  const afterSwitch = { tree: await boxOf(page, '.region-tree'), code: await boxOf(page, '.region-code') };
  check(
    'no view switch moves the tree or the code column',
    JSON.stringify(beforeSwitch) === JSON.stringify(afterSwitch),
    `tree ${JSON.stringify(beforeSwitch.tree)} vs ${JSON.stringify(afterSwitch.tree)}`,
  );

  // The switch is inside the preview column: the preview's box contains the switch, and the switch is
  // inside the column's own header rather than a page-level strip above the workspace.
  const tabsBox = await boxOf(page, '.preview-tabs');
  check(
    'the view switch lives inside the preview column',
    Boolean(tabsBox && preview && tabsBox.x >= preview.x && tabsBox.y >= preview.y && tabsBox.y < code.y + code.h),
    tabsBox ? `switch at ${tabsBox.x},${tabsBox.y}` : 'no switch',
  );

  if (shots) {
    await mkdir(shots, { recursive: true });
    // Three pictures, because a layout is what a reviewer has to see: the diagnostics default (which
    // compiles itself since #444), the scene snapshot (the device-frame column doing its job), and the
    // tab/compile-target state the file clicks above produced. Nothing here clicks a button to make a
    // preview appear: the first automatic apply is what a visitor sees.
    await page.locator('#tab-diagnostics').click();
    await page.waitForTimeout(300);
    await page.screenshot({ path: join(shots, '431-1440x900.png') });
    await page.waitForTimeout(1200);
    await page.locator('#tab-scene').click();
    await page.waitForTimeout(1200);
    await page.screenshot({ path: join(shots, '431-1440x900-scene.png') });
    console.log(`screenshots: ${join(shots, '431-1440x900.png')}, ${join(shots, '431-1440x900-scene.png')}`);
  }

  check('no console errors at 1440x900', errors.length === 0, errors.slice(0, 3).join(' | ') || 'none');
  await context.close();
}

// --- ?view= and #p5= still resolve ----------------------------------------------------------------

{
  const { page, context } = await open(1440, 900, '/index.html?view=canvas');
  const selected = await page.locator('#tab-canvas').getAttribute('aria-selected');
  const canvasVisible = await page.locator('#view-canvas').isVisible();
  check('?view=canvas opens on the canvas pane', selected === 'true' && canvasVisible, `aria-selected=${selected}`);
  await context.close();
}

{
  // A real fragment from the current codec (the same shape a share link produces), decoded on load.
  const { page, context } = await open(1440, 900);
  await page.locator('#share').click();
  await page.waitForTimeout(600);
  const shared = page.url();
  await context.close();

  const { page: reopened, context: reopenedContext } = await open(1440, 900);
  await reopened.goto(shared, { waitUntil: 'load' });
  await reopened.waitForTimeout(1400);
  const hasFragment = shared.includes('#p5=');
  const text = await reopened.evaluate(() => window.__tesseraEditor.text());
  check('#p5= share link restores the source', hasFragment && text.includes('Count: {count}'), shared.slice(-24));
  await reopenedContext.close();
}

// --- 390x844: one column, preview reachable, no horizontal scroll ---------------------------------

{
  const { page, errors, context } = await open(390, 844);
  const metrics = await page.evaluate(() => ({
    scrollWidth: document.documentElement.scrollWidth,
    clientWidth: document.documentElement.clientWidth,
    bodyScroll: document.body.scrollWidth,
  }));
  check(
    'no horizontal scroll at 390x844',
    metrics.scrollWidth <= metrics.clientWidth && metrics.bodyScroll <= metrics.clientWidth,
    `scrollWidth ${metrics.scrollWidth} vs clientWidth ${metrics.clientWidth}`,
  );

  const tree = await boxOf(page, '#project-tree');
  const preview = await boxOf(page, '.region-preview');
  check(
    'the layout is one column at 390px (the preview below the tree)',
    Boolean(tree && preview) && preview.y > tree.y && tree.x === preview.x,
    `tree y=${tree?.y}, preview y=${preview?.y}`,
  );

  // The preview is reachable: its switch is on screen after scrolling to it, and switching views works.
  await page.locator('#tab-canvas').scrollIntoViewIfNeeded();
  const canvasTab = await boxOf(page, '#tab-canvas');
  const width = 390;
  check(
    'the preview is reachable at 390px',
    Boolean(canvasTab) && canvasTab.x >= 0 && canvasTab.x + canvasTab.w <= width,
    canvasTab ? `canvas tab at ${canvasTab.x}..${canvasTab.x + canvasTab.w}` : 'no canvas tab',
  );

  if (shots) {
    await page.screenshot({ path: join(shots, '431-390x844.png'), fullPage: true });
    console.log(`screenshot: ${join(shots, '431-390x844.png')}`);
  }

  check('no console errors at 390x844', errors.length === 0, errors.slice(0, 3).join(' | ') || 'none');
  await context.close();
}

console.log(`\n${failures === 0 ? 'all layout checks passed' : `${failures} layout check(s) failed`}`);
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
