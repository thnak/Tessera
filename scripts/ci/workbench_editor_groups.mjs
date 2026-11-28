// SPDX-License-Identifier: MIT
//
// Split editor groups (#1025) in a real browser. Developer-run like the other browser gates (it needs Playwright
// and Chromium, which the repository does not depend on):
//
//   node scripts/ci/workbench_editor_groups.mjs --site <built site dir> [--chromium path]
//
// The site is designer/web/dist. No Wasm module is needed.
//
// Checks: with three files open, Split Editor shows two groups, two tab strips and two CodeMirror editors that
// show two different files; a click into either editor makes its file the page's active one; dragging a tab
// from one group to the other moves it (and Alt+Arrow does the same from the keyboard); a tab edited in one
// group keeps its unsaved dot after the move, and the Canvas element is the same DOM node throughout; closing
// the last tab of a group, or Merge, ends the split.

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';

import { ENTRY, SHEET, ensureProject, selectFile } from './studio_project.mjs';

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.wasm': 'application/wasm', '.ttf': 'font/ttf', '.txt': 'text/plain; charset=utf-8' };
const argument = (name, fallback) => {
  const index = process.argv.indexOf(name);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
};
const site = argument('--site', 'designer/web/dist');
const executablePath = argument('--chromium', undefined);

let chromium;
try {
  ({ chromium } = await import('playwright'));
} catch {
  console.error('playwright is not installed. This script is developer-run on purpose (no repository dependency):\n  npm install playwright && npx playwright install chromium');
  process.exit(2);
}

const server = createServer(async (request, response) => {
  let path = normalize(decodeURIComponent(new URL(request.url, 'http://127.0.0.1').pathname));
  if (path === '/' || path === '\\') path = '/index.html';
  try {
    const body = await readFile(join(site, path));
    response.writeHead(200, { 'content-type': MIME[extname(path).toLowerCase()] ?? 'application/octet-stream', 'cache-control': 'no-store' });
    response.end(body);
  } catch {
    response.writeHead(404, { 'content-type': 'text/plain' });
    response.end('not found');
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = 'http://127.0.0.1:' + server.address().port;
const browser = await chromium.launch({ executablePath });

let failures = 0;
const check = (name, ok, detail = '') => {
  console.log((ok ? 'ok   ' : 'FAIL ') + ' ' + name + (detail ? '  -- ' + detail : ''));
  if (!ok) failures += 1;
};

const EXTRA = 'styles/extra.css';
const page = await (await browser.newContext({ viewport: { width: 1600, height: 900 } })).newPage();
const errors = [];
page.on('console', (message) => message.type() === 'error' && errors.push(message.text()));
page.on('pageerror', (error) => errors.push('pageerror: ' + String(error)));
await page.goto(base, { waitUntil: 'load' });
await ensureProject(page, { extraFiles: [[EXTRA, 'Text { color: #ff0000; }\n']], force: true });
for (const path of [ENTRY, SHEET, EXTRA]) await selectFile(page, path);
await page.waitForFunction((path) => window.__tesseraEditor?.path === path, EXTRA);

const group = (name) => page.locator('.editor-group[data-group="' + name + '"]');
const tabsOf = (name) => group(name).locator('.editor-tab').evaluateAll((nodes) => nodes.map((node) => node.dataset.path));
const hostText = (id) => page.evaluate((host) => document.querySelector(host + ' .cm-content')?.textContent ?? '', id);
const activeOf = (name) => group(name).locator('[role=tab][aria-selected=true]').evaluate((node) => node.parentElement.dataset.path).catch(() => null);

// Tag the canvas node: it must be the same element after every split, move and merge.
const canvasTag = () => page.evaluate(() => {
  const node = document.querySelector('#canvas');
  if (!node) return 'none';
  if (!node.__groupsProbe) node.__groupsProbe = Math.random().toString(36).slice(2);
  return node.__groupsProbe;
});
const canvas0 = await canvasTag();

check('one group before a split', (await page.locator('.editor-group').count()) === 1);
check('Split is enabled with three tabs', await page.locator('#editor-split').isEnabled());
await page.locator('#editor-split').click();
await page.waitForSelector('.editor-group[data-group="side"]');

check('two groups after Split', (await page.locator('.editor-group').count()) === 2);
check('the focused tab moved: main has the other two', JSON.stringify(await tabsOf('main')) === JSON.stringify([ENTRY, SHEET]), JSON.stringify(await tabsOf('main')));
check('the second group has the split tab', JSON.stringify(await tabsOf('side')) === JSON.stringify([EXTRA]));
check('two tablists, one per group', (await page.locator('[role=tablist][aria-label^="Open files"]').count()) === 2);
await page.waitForFunction(() => (document.querySelector('#editor-host-side .cm-content')?.textContent ?? '').includes('#ff0000'));
check('the second editor shows the second file', (await hostText('#editor-host-side')).includes('#ff0000'));
check('the main editor shows a different file', !(await hostText('#editor-host')).includes('#ff0000') && (await hostText('#editor-host')).length > 0);
check('editors have distinct surface ids', (await page.locator('#source').count()) === 1 && (await page.locator('#source-side').count()) === 1);

// Focus: clicking into the main editor makes its file the page's file; typing marks that file unsaved.
await page.locator('#editor-host .cm-content').click();
await page.waitForFunction((path) => window.__tesseraEditor?.path === path, await activeOf('main'));
check('focusing the main editor makes it the page editor', (await page.evaluate(() => window.__tesseraEditor.path)) === (await activeOf('main')));
await page.evaluate(() => window.__tesseraEditor.edit(window.__tesseraEditor.text() + '\n// typed in the main group\n'));
await page.waitForTimeout(200);
const typedPath = await activeOf('main');
const dirtyOf = (path) => page.locator('.editor-tab[data-path="' + path + '"] .editor-tab-dirty').count();
check('the edited tab shows its unsaved dot', (await dirtyOf(typedPath)) === 1, typedPath);
check('the other group\'s tab is not dirty', (await dirtyOf(EXTRA)) === 0);

// Drag the dirty tab to the second group.
await group('main').locator('.editor-tab[data-path="' + typedPath + '"]').dragTo(group('side').locator('.tabrow'));
await page.waitForTimeout(250);
check('the dragged tab is in the second group', (await tabsOf('side')).includes(typedPath), JSON.stringify(await tabsOf('side')));
check('and gone from the main group', !(await tabsOf('main')).includes(typedPath));
check('it kept its unsaved dot', (await dirtyOf(typedPath)) === 1);
await page.waitForFunction((path) => (document.querySelector('#editor-host-side .cm-content')?.textContent ?? '').includes('typed in the main group') || path === '', typedPath);
check('and its edited text', (await hostText('#editor-host-side')).includes('typed in the main group'));
check('the canvas element is untouched', (await canvasTag()) === canvas0);

// The keyboard twin: Alt+ArrowLeft on the focused tab moves it back.
await group('side').locator('.editor-tab[data-path="' + typedPath + '"] [role=tab]').focus();
await page.keyboard.press('Alt+ArrowLeft');
await page.waitForTimeout(250);
check('Alt+ArrowLeft moves the tab back to the main group', (await tabsOf('main')).includes(typedPath), JSON.stringify(await tabsOf('main')));
check('still dirty after the round trip', (await dirtyOf(typedPath)) === 1);

// Close the second group's only tab: the split ends.
await group('side').locator('.editor-tab-close').first().click();
await page.waitForTimeout(250);
check('closing the last second-group tab ends the split', (await page.locator('.editor-group').count()) === 1);

// Split again, then Merge.
await selectFile(page, SHEET);
await page.locator('#editor-split').click();
await page.waitForSelector('.editor-group[data-group="side"]');
await page.locator('#editor-join').click();
await page.waitForTimeout(250);
check('Merge ends the split', (await page.locator('.editor-group').count()) === 1);
check('the canvas element is still the same node', (await canvasTag()) === canvas0);
check('no console errors', errors.filter((entry) => !/Failed to load resource/.test(entry)).length === 0, errors.slice(0, 3).join(' | '));

console.log(failures === 0 ? '\nall editor group checks passed' : '\n' + failures + ' check(s) failed');
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
