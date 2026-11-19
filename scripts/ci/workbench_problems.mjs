// SPDX-License-Identifier: MIT
//
// The owner's studio decisions in a real browser: Share is gone, the two hidden editor lines are gone, and
// Diagnostics is a real dock panel (Problems) with split counts in the status bar. Developer-run like the other
// browser gates (it needs Playwright and Chromium, which the repository does not depend on):
//
//   node scripts/ci/workbench_problems.mjs --site <built site dir> [--shots out-dir] [--chromium path]
//
// The site is `designer/web/dist`. The compile checks need the Wasm modules next to it (copy
// `tessera_diagnostics.{js,wasm}` from a Pages build); without them those checks are reported as skipped, and
// the rest (which publish diagnostics through the `__tesseraProblems` seam) still run.
//
// Checks: share gone (button, menu, palette, a leftover `#p5=` link); no hidden hint lines; Problems is a
// bottom-dock tab of a collapsed panel area; the status item shows split error and warning counts and opens
// and focuses the panel; the filter chips; click-to-jump (same file and another file); a hidden panel
// subscribes to nothing; a version 1 stored layout migrates; the panel is a normal dock tab (movable).

import { createServer } from 'node:http';
import { existsSync } from 'node:fs';
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
const haveWasm = existsSync(join(site, 'tessera_diagnostics.wasm'));

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
const base = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch({ executablePath });

let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok   ' : 'FAIL '} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
};
const skip = (name) => console.log(`skip  ${name}  -- needs tessera_diagnostics.wasm next to the site`);

async function open({ width = 1440, height = 900, scheme = 'light', url = base, storage = null } = {}) {
  const context = await browser.newContext({ viewport: { width, height }, colorScheme: scheme });
  if (storage !== null) {
    await context.addInitScript((entries) => {
      for (const [key, value] of Object.entries(entries)) {
        if (localStorage.getItem(key) === null) localStorage.setItem(key, value);
      }
    }, storage);
  }
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (error) => errors.push(String(error)));
  await page.goto(url, { waitUntil: 'load' });
  await page.waitForSelector('.flexlayout__layout');
  await page.waitForFunction(() => window.__tesseraWorkbench?.layout && window.__tesseraProblems);
  await page.waitForTimeout(500);
  return { page, context, errors };
}

const layout = (page) => page.evaluate(() => window.__tesseraWorkbench.layout());
const groupOf = (doc, id) => {
  const walk = (node) => (node.type === 'group' ? (node.panels.includes(id) ? node : null) : node.children.map(walk).find(Boolean) ?? null);
  return walk(doc.root);
};
const listeners = (page) => page.evaluate(() => window.__tesseraProblems.listenerCount());
// Quick Open: the one way to open a second file without knowing the tree's expanded state.
const quickOpen = async (page, text) => {
  await page.keyboard.press('Control+p');
  await page.waitForSelector('#palette-input');
  await page.keyboard.type(text);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(400);
};
const publish = (page, diagnostics) => page.evaluate((list) => window.__tesseraProblems.publish(list), diagnostics);
const counts = (page) => page.evaluate(() => ({
  errors: document.getElementById('status-errors')?.dataset.count,
  warnings: document.getElementById('status-warnings')?.dataset.count,
  errorText: document.querySelector('#status-errors .status-problem-count')?.textContent,
  warningText: document.querySelector('#status-warnings .status-problem-count')?.textContent,
  label: document.getElementById('status-diagnostics-toggle')?.getAttribute('aria-label'),
  pressed: document.getElementById('status-diagnostics-toggle')?.getAttribute('aria-pressed'),
}));
const activeTab = (page) => page.evaluate(() => document.querySelector('.editor-tab-select[aria-selected="true"]')?.title ?? '');
const encoder = new TextEncoder();
const zipOf = (files) => Buffer.from(writeZip(Object.entries(files).map(([path, text]) => ({ path, bytes: encoder.encode(text) }))));
const PROJECT = {
  'tessera.json': '{"schemaVersion":1,"app":{"name":"p","entry":"ui/Home.tsx","namespace":"p"}}',
  'ui/Home.tsx': 'export function Home(): Node {\n  return <View><Text>hi</Text></View>;\n}\n',
  'ui/Other.tsx': 'export function Other(): Node {\n  const total = 1;\n  return <View><Text>{total}</Text></View>;\n}\n',
};
const openProject = async (page, files) => {
  await page.locator('input[type=file]').setInputFiles({ name: 'p.zip', mimeType: 'application/zip', buffer: zipOf(files) });
  await page.waitForSelector('#project-name');
  await page.waitForTimeout(800);
};
const SAMPLE_PROBLEMS = [
  { severity: 'error', code: 'TESSERA_TEST_ERROR', message: 'an error in Other', span: { file: 'ui/Other.tsx', line: 2, column: 9, offset: 36, length: 5 } },
  { severity: 'warning', code: 'TESSERA_TEST_WARNING', message: 'a warning in Home', span: { file: 'ui/Home.tsx', line: 2, column: 10, offset: 40, length: 4 } },
  { severity: 'warning', code: 'TESSERA_TEST_WARNING_TWO', message: 'another warning in Home', span: { file: 'ui/Home.tsx', line: 3, column: 1, offset: 70, length: 1 } },
];

// --- 1. Share is gone ------------------------------------------------------------------------------------
{
  const { page, context, errors } = await open({ url: `${base}/index.html#p5=uAAAA` });
  check('no #share toolbar button', (await page.locator('#share').count()) === 0);
  check('no share-status line and no broken-link banner (a leftover #p5= link is ignored)', (await page.locator('#share-status, #link-note').count()) === 0);
  check('the toolbar is Restart, Save, the device chip and the palette', (await page.locator('[role=toolbar] button, [role=toolbar] .device-chip').count()) >= 3 && (await page.locator('[role=toolbar]').innerText()).toLowerCase().includes('share') === false);
  await page.getByRole('menuitem', { name: 'File', exact: true }).first().click();
  await page.waitForSelector('[role=menu]');
  const labels = await page.$$eval('[role=menu] [role^=menuitem]', (nodes) => nodes.map((node) => node.textContent.trim()));
  check('the File menu has no share item', !labels.some((label) => /share/i.test(label)), labels.join(' | '));
  check('File > Open Sample is still there', labels.some((label) => label.startsWith('Open Sample')));
  await page.keyboard.press('Escape');
  await page.keyboard.press('Control+Shift+P');
  await page.waitForSelector('#palette-input');
  await page.keyboard.type('share');
  const options = await page.$$eval('#palette-list [role=option]', (nodes) => nodes.map((node) => node.textContent.trim()));
  check('the palette finds no Share command', !options.some((option) => /share/i.test(option)), options.join(' | '));
  await page.keyboard.press('Escape');
  const verdict = await page.evaluate(() => window.__tesseraWorkbench.run('file.share'));
  check('the retired file.share id is refused as removed', verdict?.ok === false && verdict?.code === 'removed', JSON.stringify(verdict));
  await page.keyboard.press('Control+Shift+P');
  await page.waitForSelector('#palette-input');
  await page.keyboard.type('theme');
  const themes = await page.$$eval('#palette-list [role=option]', (nodes) => nodes.map((node) => node.textContent.trim()));
  check('the theme command stays in the palette (and View > Appearance)', themes.some((option) => /theme/i.test(option)), themes.join(' | '));
  await page.keyboard.press('Escape');
  check('no uncaught page error (boot)', errors.length === 0, errors.join(' | '));
  await context.close();
}

// --- 2. no hidden hint lines -------------------------------------------------------------------------------
{
  const { page, context } = await open();
  await openProject(page, PROJECT);
  check('#editor-hint and #editor-file are gone with a project open', (await page.locator('#editor-hint, #editor-file').count()) === 0);
  check('the code column has no visually hidden hint paragraph', (await page.locator('[data-panel=editor] p.visually-hidden').count()) === 0);
  const tab = await page.locator('.editor-tab-select[aria-selected="true"]').getAttribute('title');
  check('the active tab names the file (title and aria-selected replace the hidden line)', tab !== null && tab.endsWith('.tsx'), String(tab));
  await context.close();
}

// --- 3. Problems is a bottom-dock tab of a collapsed panel area ---------------------------------------------
{
  const { page, context, errors } = await open();
  const doc = await layout(page);
  const bottom = groupOf(doc, 'problems');
  check('the layout is version 2 and Problems is the first tab of the bottom group', doc.version === 2 && bottom?.id === 'g-bottom' && bottom.panels[0] === 'problems' && bottom.panels[1] === 'diagnostics', JSON.stringify(bottom));
  check('the bottom dock is collapsed by default', doc.collapsed.includes('g-bottom') && (await page.locator('#problems-panel:visible').count()) === 0);
  const before = await counts(page);
  check('before the first compile the status item says not run yet (dashes, one label)', before.errorText === '–' && before.warningText === '–' && /not run yet/.test(before.label), JSON.stringify(before));
  check('two icons: error and warning', (await page.locator('#status-errors [aria-hidden]').innerText()) === '⊗' && (await page.locator('#status-warnings [aria-hidden]').innerText()) === '⚠');
  const baseline = await listeners(page);
  check('a hidden Problems panel subscribes to nothing (the status item and the auto-open watcher listen)', baseline === 2, String(baseline));
  await publish(page, SAMPLE_PROBLEMS);
  await page.waitForTimeout(150);
  const split = await counts(page);
  check('split counts come from the severity of each diagnostic: 1 error, 2 warnings', split.errors === '1' && split.warnings === '2' && split.errorText === '1' && split.warningText === '2' && /1 error, 2 warnings/.test(split.label), JSON.stringify(split));
  check('publishing while the panel is hidden still costs the panel nothing', (await listeners(page)) === baseline && (await page.locator('#problems-panel:visible').count()) === 0);

  await page.click('#status-diagnostics-toggle');
  await page.waitForSelector('#problems-panel:visible');
  await page.waitForTimeout(250);
  const doc2 = await layout(page);
  check('clicking the status item opens the bottom dock on the Problems tab', !doc2.collapsed.includes('g-bottom') && groupOf(doc2, 'problems').active === 'problems');
  check('  the status item is pressed and the panel subscribed', (await counts(page)).pressed === 'true' && (await listeners(page)) === baseline + 1);
  const focusedIn = await page.evaluate(() => document.getElementById('problems-panel')?.contains(document.activeElement));
  check('  keyboard focus moved into the panel', focusedIn === true);
  const rows = await page.$$eval('#problems-list [data-problem]', (nodes) => nodes.map((node) => ({ severity: node.dataset.severity, code: node.querySelector('.problems-code').textContent, where: node.querySelector('.problems-where').textContent, icon: node.querySelector('.problems-icon').getAttribute('aria-label'), message: node.querySelector('.problems-message').textContent })));
  check('the list shows severity icon, code, message and file:line:col, in order', rows.length === 3 && rows[0].code === 'TESSERA_TEST_ERROR' && rows[0].where === 'ui/Other.tsx:2:9' && rows[0].icon === 'Error' && rows[1].icon === 'Warning' && rows[1].where === 'ui/Home.tsx:2:10' && rows[0].message === 'an error in Other', JSON.stringify(rows));
  check('  the panel is a dock tab with a tab button', (await page.locator('#flexlayout-tabbutton-problems').count()) === 1);

  // filters
  await page.click('[data-problems-filter=error]');
  check('Errors off: only the warnings remain', (await page.locator('#problems-list [data-problem]').count()) === 2 && (await page.getAttribute('[data-problems-filter=error]', 'aria-pressed')) === 'false');
  await page.click('[data-problems-filter=warning]');
  check('both off: an empty message, no list', (await page.locator('#problems-list').count()) === 0 && /match the filter/.test(await page.locator('.problems-empty').innerText()));
  await page.click('[data-problems-filter=error]');
  await page.click('[data-problems-filter=warning]');
  check('filters back on: three rows again', (await page.locator('#problems-list [data-problem]').count()) === 3);
  check('the filter chips carry the counts', (await page.locator('[data-problems-filter=error] .problems-count').innerText()) === '1' && (await page.locator('[data-problems-filter=warning] .problems-count').innerText()) === '2' && (await page.locator('[data-problems-filter=info] .problems-count').innerText()) === '0');

  // keyboard
  await page.locator('#problems-list [data-problem]').first().focus();
  await page.keyboard.press('ArrowDown');
  check('Down moves between rows', (await page.evaluate(() => document.activeElement?.dataset.problem)) === '1');
  await page.keyboard.press('Control+Shift+m');
  await page.waitForTimeout(300);
  check('Ctrl+Shift+M from inside the panel hides it (view.problems toggles)', (await page.locator('#problems-panel:visible').count()) === 0 && (await listeners(page)) === baseline);
  await page.keyboard.press('Control+Shift+m');
  await page.waitForSelector('#problems-panel:visible');
  await page.waitForTimeout(250);
  check('Ctrl+Shift+M again shows and focuses it', await page.evaluate(() => document.getElementById('problems-panel')?.contains(document.activeElement)));
  const ran = await page.evaluate(() => window.__tesseraWorkbench.run('view.problems'));
  check('the command id view.problems is live (palette and menu run the same handler)', ran?.ok === true, JSON.stringify(ran));
  if (shots) {
    await mkdir(shots, { recursive: true });
    await page.screenshot({ path: join(shots, 'problems-light-open.png') });
  }
  check('no uncaught page error (problems)', errors.length === 0, errors.join(' | '));
  await context.close();
}

// --- 4. click to jump ---------------------------------------------------------------------------------------
{
  const { page, context } = await open();
  await openProject(page, PROJECT);
  await quickOpen(page, 'Other');
  await publish(page, SAMPLE_PROBLEMS);
  await page.click('#status-diagnostics-toggle');
  await page.waitForSelector('#problems-panel:visible');
  await page.waitForTimeout(250);
  await page.locator('#problems-list [data-problem]').nth(1).click();
  await page.waitForTimeout(500);
  check('clicking a problem in another file opens that file', (await activeTab(page)) === 'ui/Home.tsx', await activeTab(page));
  const caret = await page.locator('#status-caret').innerText();
  check('  the span is selected and the editor has focus (Ln 2, 4 selected)', /Ln 2, Col \d+ \(4 selected\)/.test(caret) && (await page.evaluate(() => document.activeElement?.closest('#editor-host') !== null)), caret);
  await page.evaluate(() => { document.querySelector('#flexlayout-tabbutton-problems')?.click(); });
  await page.waitForTimeout(250);
  await page.locator('#problems-list [data-problem]').nth(2).click();
  await page.waitForTimeout(400);
  const same = await page.locator('#status-caret').innerText();
  check('clicking a problem in the open file moves the selection there (Ln 3, 1 selected)', /Ln 3, Col \d+ \(1 selected\)/.test(same), same);
  await page.evaluate(() => window.__tesseraProblems.publish([{ severity: 'error', code: 'NO_OFFSET', message: 'line only', span: { file: 'ui/Home.tsx', line: 3, column: 1 } }]));
  await page.waitForTimeout(150);
  await page.locator('#problems-list [data-problem]').first().click();
  await page.waitForTimeout(300);
  check('a problem with no offset jumps by line', /Ln 3, Col 1\b/.test(await page.locator('#status-caret').innerText()), await page.locator('#status-caret').innerText());
  await context.close();
}

// --- 5. a real compile reaches the panel (needs the Wasm module) ----------------------------------------
if (!haveWasm) {
  skip('a real diagnostics run fills the status counts and the Problems list');
} else {
  const { page, context } = await open();
  const files = { ...PROJECT, 'ui/Home.tsx': 'export function Home(): Node {\n  return <View bogus="1"><Text>hi</Text></View>;\n}\n' };
  await openProject(page, files);
  await page.waitForFunction(() => window.__tesseraProblems.get().ran, null, { timeout: 15000 });
  const real = await counts(page);
  check('a real compile: the status item counts the error (1, 0)', real.errors === '1' && real.warnings === '0' && /1 error, 0 warnings/.test(real.label), JSON.stringify(real));
  await quickOpen(page, 'Other');
  await page.click('#status-diagnostics-toggle');
  await page.waitForSelector('#problems-panel:visible');
  await page.waitForTimeout(250);
  const row = page.locator('#problems-list [data-problem]').first();
  check('  the row names the compiler code and the file position', (await row.locator('.problems-code').innerText()).startsWith('TESSERA_') && (await row.locator('.problems-where').innerText()) === 'ui/Home.tsx:2:16', await row.innerText());
  await row.click();
  await page.waitForTimeout(500);
  check('  clicking it opens ui/Home.tsx and selects the span', (await activeTab(page)) === 'ui/Home.tsx' && /selected/.test(await page.locator('#status-caret').innerText()), `${await activeTab(page)} ${await page.locator('#status-caret').innerText()}`);
  await page.evaluate(() => window.__tesseraEditor.edit('export function Home(): Node {\n  return <View><Text>hi</Text></View>;\n}\n'));
  await page.waitForFunction(() => window.__tesseraProblems.get().errors === 0, null, { timeout: 15000 });
  await page.waitForTimeout(250);
  check('  fixing the source clears the list and the counts', (await counts(page)).errors === '0' && /No problems/.test(await page.locator('.problems-empty').innerText()));
  await context.close();
}

// --- 6. layout migration and movability ---------------------------------------------------------------------
{
  const v1 = {
    format: 'tessera.studio.layout', version: 1,
    root: { type: 'split', dir: 'row', sizes: [0.18, 0.82], children: [
      { type: 'group', id: 'g-project', panels: ['project'], active: 'project' },
      { type: 'split', dir: 'column', sizes: [0.72, 0.28], children: [
        { type: 'split', dir: 'row', sizes: [0.5, 0.5], children: [
          { type: 'group', id: 'g-editor', panels: ['editor'], active: 'editor' },
          { type: 'group', id: 'g-preview', panels: ['canvas', 'scene'], active: 'canvas' },
        ] },
        { type: 'group', id: 'g-bottom', panels: ['diagnostics', 'mock-console', 'call-trace', 'platform-events'], active: 'diagnostics' },
      ] },
    ] },
    maximized: null, closed: [], shell: { activityBar: true, statusBar: true, toolbar: true, sideBar: 'left', zen: false }, collapsed: ['g-bottom'],
  };
  const { page, context, errors } = await open({ storage: { 'tessera.studio.layout': JSON.stringify(v1) } });
  const doc = await layout(page);
  const bottom = groupOf(doc, 'problems');
  check('a stored version 1 layout migrates to version 2 with Problems first in its Diagnostics group', doc.version === 2 && bottom?.id === 'g-bottom' && bottom.panels.join() === 'problems,diagnostics,mock-console,call-trace,platform-events', JSON.stringify(bottom));
  check('  the viewer\'s active tab and collapsed state are kept, with no notification about it', bottom.active === 'diagnostics' && doc.collapsed.includes('g-bottom') && (await page.locator('#notification-live .toast').count()) === 0);
  check('no uncaught page error (migration)', errors.length === 0, errors.join(' | '));
  await context.close();
}
{
  const { page, context } = await open();
  await page.evaluate(() => window.__tesseraWorkbench.run('view.movePanel', { id: 'problems', to: 'right' }));
  await page.waitForTimeout(500);
  const doc = await layout(page);
  check('Problems moves like any dock tab (view.movePanel to the right edge)', groupOf(doc, 'problems').id !== 'g-bottom' && doc.root.children.at(-1)?.panels?.[0] === 'problems', JSON.stringify(doc.root.children.at(-1)));
  await page.click('#status-diagnostics-toggle');
  await page.waitForSelector('#problems-panel:visible');
  check('  and still opens and focuses from the status bar in its new place', await page.evaluate(() => document.getElementById('problems-panel')?.getBoundingClientRect().x > 900));
  await page.evaluate(() => window.__tesseraWorkbench.run('view.togglePanel', { id: 'problems', close: true }));
  await page.waitForTimeout(300);
  check('  closing it releases the panel (no listener) and the status item still counts', (await listeners(page)) === 2 && (await counts(page)).pressed === 'false');
  await context.close();
}

// --- 7. screenshots ------------------------------------------------------------------------------------------
if (shots !== null) {
  await mkdir(shots, { recursive: true });
  for (const scheme of ['light', 'dark']) {
    const { page, context } = await open({ scheme });
    await openProject(page, PROJECT);
    await publish(page, SAMPLE_PROBLEMS);
    await page.click('#status-diagnostics-toggle');
    await page.waitForSelector('#problems-panel:visible');
    await page.waitForTimeout(400);
    await page.screenshot({ path: join(shots, `problems-${scheme}.png`) });
    await context.close();
  }
}

await browser.close();
server.close();
console.log(failures === 0 ? '\nproblems browser check: all passed' : `\nproblems browser check: ${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
