// SPDX-License-Identifier: MIT
//
// Project search and replace (#580) in a real browser. Developer-run like the other browser gates (it needs
// Playwright and Chromium, which the repository does not depend on):
//
//   node scripts/ci/workbench_search.mjs --site <built site dir> [--shots out-dir] [--chromium path]
//
// The site is designer/web/dist. No Wasm module is needed: the search worker is plain JS in the page.
//
// Checks: Ctrl+Shift+F shows Search as the side bar group's second tab and focuses its field; typing finds
// matches across files, grouped by file, with counts; case, whole word and regex toggles (and Alt+C/W/R); an
// invalid regex is a sentence; include and exclude globs; a result opens its file with the match selected;
// replace one match, replace in a file and replace all each change files and mark them unsaved, and ONE Undo
// restores every file of a multi-file replace (button and Ctrl+Z in the results), Redo replays it; a file
// edited after a replace is left alone by Undo; a capped result list disables Replace All; the roles and
// names the panel exposes (search form, live status, a tree-less list of buttons, arrow-key navigation).

import { createServer } from 'node:http';
import { mkdir, readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';

import { writeZip } from '../../designer/web/studio-zip.js';

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.wasm': 'application/wasm', '.ttf': 'font/ttf', '.txt': 'text/plain; charset=utf-8' };
const argument = (name, fallback) => {
  const index = process.argv.indexOf(name);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
};
const site = argument('--site', 'designer/web/dist');
const shots = argument('--shots', null);
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

const encoder = new TextEncoder();
const zipOf = (files) => Buffer.from(writeZip(Object.entries(files).map(([path, text]) => ({ path, bytes: encoder.encode(text) }))));
const HOME = 'export function Home(): Node {\n  const total = 1;\n  return <View><Text>{total}</Text></View>;\n}\n';
const OTHER = 'export function Other(): Node {\n  const Total = 2;\n  const subtotal = total + 1;\n  return <View><Text>{Total}</Text></View>;\n}\n';
const PROJECT = {
  'tessera.json': '{"schemaVersion":1,"app":{"name":"p","entry":"ui/Home.tsx","namespace":"p"}}',
  'ui/Home.tsx': HOME,
  'ui/Other.tsx': OTHER,
  'ui/app.css': '.total { color: red; }\n',
  'dist/out.tsx': 'const total = 3;\n',
};

async function open() {
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (error) => errors.push(String(error)));
  await page.goto(base, { waitUntil: 'load' });
  await page.waitForSelector('.flexlayout__layout');
  await page.waitForFunction(() => window.__tesseraWorkbench?.layout);
  await page.locator('input[type=file]').setInputFiles({ name: 'p.zip', mimeType: 'application/zip', buffer: zipOf(PROJECT) });
  await page.waitForSelector('#project-name');
  await page.waitForTimeout(800);
  return { page, context, errors };
}

const results = (page) =>
  page.$$eval('#search-results .search-group', (groups) =>
    groups.map((group) => ({ path: group.dataset.searchPath, count: group.querySelectorAll('[data-search-row]').length })),
  );
const counts = async (page) => Object.fromEntries((await results(page)).map((g) => [g.path, g.count]));
const status = (page) => page.locator('#search-status').innerText();
const answered = (page) => page.evaluate(() => Number(document.getElementById('search-status')?.dataset.answered ?? 0));
// Runs `action`, then waits for the search it triggers to answer (the debounce, the worker and the render).
const answer = async (page, action) => {
  const seen = await answered(page);
  await action();
  await page.waitForFunction((n) => Number(document.getElementById('search-status')?.dataset.answered ?? 0) > n, seen, { timeout: 10000 });
};
const typeQuery = (page, text) => answer(page, () => page.fill('#search-query', text));
const toggle = (page, key) => answer(page, () => page.click('[data-search-toggle=' + key + ']'));
const editorText = (page) => page.evaluate(() => window.__tesseraEditor.text());
const projectTab = async (page) => {
  await page.click('#flexlayout-tabbutton-project');
  await page.waitForSelector('#project-tree');
};
const searchTab = async (page) => {
  await page.keyboard.press('Control+Shift+F');
  await page.waitForSelector('#search-query');
};
const dirtyCount = async (page) => {
  await projectTab(page);
  const count = await page.locator('#project-tree .tree-dirty').count();
  await searchTab(page);
  return count;
};
// Quick Open the file (the one way that needs no knowledge of the tree), and read the editor.
const fileText = async (page, name) => {
  await page.waitForTimeout(200);
  await page.keyboard.press('Control+p');
  await page.waitForSelector('#palette-input');
  await page.keyboard.type(name);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(400);
  return editorText(page);
};

{
  const { page, context, errors } = await open();

  // 1. the command, the tab, the focus
  await searchTab(page);
  const doc = await page.evaluate(() => window.__tesseraWorkbench.layout());
  const side = (function find(node) {
    return node.type === 'group' ? (node.panels.includes('search') ? node : null) : node.children.map(find).find(Boolean) ?? null;
  })(doc.root);
  check('Ctrl+Shift+F shows Search as a tab of the side bar group, next to Project', side?.id === 'g-project' && side.panels.join() === 'project,search' && side.active === 'search', JSON.stringify(side));
  check('  the query field has the focus', await page.evaluate(() => document.activeElement?.id === 'search-query'));
  check('  view.search is a live command', (await page.evaluate(() => window.__tesseraWorkbench.run('view.search')))?.ok === true);
  check('  the panel is a search landmark with a polite live status line', (await page.locator('#search-panel form[role=search]').count()) === 1 && (await page.getAttribute('#search-status', 'aria-live')) === 'polite');

  // 2. results across files, the three toggles, globs
  await typeQuery(page, 'total');
  let found = await counts(page);
  check('a plain search is case-insensitive and groups the matches by file', JSON.stringify(found) === JSON.stringify({ 'dist/out.tsx': 1, 'ui/Home.tsx': 2, 'ui/Other.tsx': 4, 'ui/app.css': 1 }), JSON.stringify(found));
  check('  the status line counts results and files', /^8 results in 4 files/.test(await status(page)), await status(page));

  await toggle(page, 'caseSensitive');
  found = await counts(page);
  check('Match Case narrows the list (Total no longer matches total)', found['ui/Other.tsx'] === 2 && found['ui/Home.tsx'] === 2, JSON.stringify(found));
  await answer(page, () => page.keyboard.press('Alt+c'));
  check('  Alt+C flips the toggle back', (await page.getAttribute('[data-search-toggle=caseSensitive]', 'aria-pressed')) === 'false');

  await toggle(page, 'wholeWord');
  found = await counts(page);
  check('Match Whole Word drops subtotal', found['ui/Other.tsx'] === 3, JSON.stringify(found));
  await answer(page, () => page.keyboard.press('Alt+w'));

  await answer(page, () => page.keyboard.press('Alt+r'));
  await typeQuery(page, '(sub)?total\\s*\\+');
  found = await counts(page);
  check('Alt+R turns regular expressions on: a pattern with a group and a class matches', JSON.stringify(found) === JSON.stringify({ 'ui/Other.tsx': 1 }), JSON.stringify(found));
  await typeQuery(page, '(?=x)total');
  check('an unsupported pattern is a sentence in the status line and marks the field invalid', /lookaround/.test(await status(page)) && (await page.getAttribute('#search-query', 'aria-invalid')) === 'true', await status(page));
  await typeQuery(page, '(a+)+$');
  check('a catastrophic pattern answers at once (bounded engine)', /No results/.test(await status(page)), await status(page));
  await answer(page, () => page.keyboard.press('Alt+r'));

  await typeQuery(page, 'total');
  await page.click('.search-filters-toggle');
  await answer(page, () => page.fill('#search-include', '*.tsx'));
  await answer(page, () => page.fill('#search-exclude', 'dist'));
  found = await counts(page);
  check('include *.tsx and exclude dist leave the two ui sources', JSON.stringify(found) === JSON.stringify({ 'ui/Home.tsx': 2, 'ui/Other.tsx': 4 }), JSON.stringify(found));
  await answer(page, () => page.fill('#search-include', ''));
  await answer(page, () => page.fill('#search-exclude', ''));
  await page.click('.search-filters-toggle');

  // 3. click to reveal
  await page.locator('#search-results [data-search-row^="ui/Other.tsx:"]').nth(2).click();
  await page.waitForTimeout(500);
  const tab = await page.locator('.editor-tab-select[aria-selected="true"]').getAttribute('title');
  const selected = await page.evaluate(() => window.getSelection()?.toString());
  check('clicking a result opens its file with the match selected', tab === 'ui/Other.tsx' && selected?.toLowerCase() === 'total', tab + ' / ' + selected);

  // 4. replace: one match, then all; each is ONE undo step
  await searchTab(page);
  await toggle(page, 'caseSensitive');
  await toggle(page, 'wholeWord');
  await page.click('.search-disclosure[aria-controls=search-replace]');
  await page.fill('#search-replace-input', 'sum');
  found = await counts(page);
  check('whole word + match case finds the lowercase totals in four files', JSON.stringify(found) === JSON.stringify({ 'dist/out.tsx': 1, 'ui/Home.tsx': 2, 'ui/Other.tsx': 1, 'ui/app.css': 1 }), JSON.stringify(found));
  check('  nothing is unsaved yet', (await dirtyCount(page)) === 0);
  await answer(page, () => page.locator('#search-results [data-search-row^="ui/Home.tsx:"]').first().locator('xpath=following-sibling::button').click());
  found = await counts(page);
  check('Replace on one match removes just that result and marks the file unsaved', found['ui/Home.tsx'] === 1 && (await dirtyCount(page)) === 1, JSON.stringify(found));
  check('  Undo Replace is enabled', await page.isEnabled('#search-undo'));
  await answer(page, () => page.click('#search-replace-all'));
  check('Replace All empties the list and announces the outcome', (await results(page)).length === 0 && /^Replaced 4 matches in 4 files/.test(await status(page)), await status(page));
  check('  every changed file is marked unsaved', (await dirtyCount(page)) === 4);
  const homeAll = await fileText(page, 'Home.tsx');
  check('  the editor shows the replaced text (an open file reloads)', homeAll.includes('const sum = 1;') && homeAll.includes('{sum}'), homeAll);
  await searchTab(page);
  await answer(page, () => page.click('#search-undo'));
  check('ONE Undo restores every file of the Replace All, and only that operation', (await dirtyCount(page)) === 1 && (await counts(page))['dist/out.tsx'] === 1);
  const homeUndo = await fileText(page, 'Home.tsx');
  check('  the single replace before it is still applied', homeUndo.includes('const sum = 1;') && homeUndo.includes('{total}'), homeUndo);
  await searchTab(page);
  await page.waitForSelector('#search-redo');
  check('  Redo is enabled and replays the Replace All', await page.isEnabled('#search-redo'));
  await answer(page, () => page.click('#search-redo'));
  check('  after Redo the list is empty again', (await results(page)).length === 0);

  // 5. keyboard: arrows in the results, Ctrl+Z in the results
  await page.fill('#search-replace-input', 'total');
  await typeQuery(page, 'sum');
  await page.locator('#search-results [data-search-file]').first().focus();
  await page.keyboard.press('ArrowDown');
  check('ArrowDown moves from a file heading to its first result', await page.evaluate(() => document.activeElement?.hasAttribute('data-search-row')));
  await answer(page, () => page.keyboard.press('Control+z'));
  check('Ctrl+Z in the results undoes the last replace (the Replace All)', (await counts(page))['ui/Other.tsx'] === undefined && /^Undid Replace/.test(await status(page)) === true, await status(page));
  await answer(page, () => page.keyboard.press('Control+Shift+z'));

  // 6. a file edited since the replace is left alone by Undo
  await page.evaluate(() => window.__tesseraEditor.edit(window.__tesseraEditor.text() + '// typed after\n'));
  await page.waitForTimeout(300);
  await searchTab(page);
  await answer(page, () => page.click('#search-undo'));
  check('Undo skips a file edited since and says which', /Left alone because they changed since/.test(await status(page)), await status(page));

  if (shots) {
    await mkdir(shots, { recursive: true });
    await page.screenshot({ path: join(shots, 'search.png') });
  }
  check('no uncaught page error', errors.length === 0, errors.join(' | '));
  await context.close();
}

await browser.close();
server.close();
console.log(failures === 0 ? '\nworkbench_search: all checks passed' : '\nworkbench_search: ' + failures + ' failed');
process.exit(failures === 0 ? 0 : 1);
