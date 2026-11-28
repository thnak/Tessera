// SPDX-License-Identifier: MIT
//
// The theme and design-token editor (#582) in a real browser. Developer-run like the other browser gates (it
// needs Playwright and Chromium, which the repository does not depend on):
//
//   node scripts/ci/workbench_theme.mjs --site <built site dir> [--chromium path]
//
// Checks: the Theme Tokens command shows the panel; a stylesheet of custom properties lists its variants side
// by side with contrast verdicts as text; changing a token writes the edit into the file (comments and order
// untouched) and marks it unsaved; the panel's Undo button reverts it; a low-contrast value turns a verdict to
// fail; a value that is not a color is refused with a sentence and the file is unchanged. The canvas repaint
// needs the Wasm module, so it is covered by the live-edit wiring gate (a stylesheet edit is a live apply).

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';

import { writeZip } from '../../designer/web/studio-zip.js';

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

const THEME = [
  '/* Brand tokens. */',
  ':root {',
  '  --surface: #ffffff;',
  '  --on-surface: #14171a; /* body text */',
  '  --accent: #0b5cad;',
  '}',
  ':root[data-theme="dark"] {',
  '  --surface: #101214;',
  '  --on-surface: #eceff2;',
  '  --accent: #5aa2f0;',
  '}',
  '',
].join('\n');
const encoder = new TextEncoder();
const PROJECT = {
  'tessera.json': '{"schemaVersion":1,"app":{"name":"p","entry":"ui/Home.tsx","namespace":"p"}}',
  'ui/Home.tsx': 'export function Home(): Node {\n  return <View><Text>hi</Text></View>;\n}\n',
  'ui/theme.css': THEME,
};
const zip = Buffer.from(writeZip(Object.entries(PROJECT).map(([path, text]) => ({ path, bytes: encoder.encode(text) }))));

const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
const page = await context.newPage();
const errors = [];
page.on('pageerror', (error) => errors.push(String(error)));
await page.goto(base, { waitUntil: 'load' });
await page.waitForSelector('.flexlayout__layout');
await page.waitForFunction(() => window.__tesseraWorkbench?.layout);
await page.locator('input[type=file]').setInputFiles({ name: 'p.zip', mimeType: 'application/zip', buffer: zip });
await page.waitForSelector('#project-name');
await page.waitForTimeout(800);

const bufferText = async () => {
  await page.keyboard.press('Control+p');
  await page.waitForSelector('#palette-input');
  await page.keyboard.type('theme.css');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(400);
  return page.evaluate(() => window.__tesseraEditor.text());
};

check('the Theme Tokens command is live', (await page.evaluate(() => window.__tesseraWorkbench.run('view.theme')))?.ok === true);
await page.waitForSelector('#theme-panel .theme-table');
const columns = await page.$$eval('#theme-panel thead th', (cells) => cells.map((cell) => cell.textContent));
check('the variants sit side by side', columns.join() === 'Token,light,dark', columns.join());
const verdicts = await page.$$eval('.theme-pair-verdict', (cells) => cells.map((cell) => cell.textContent));
check('contrast verdicts are words and all pass for the starting theme', verdicts.length > 0 && verdicts.every((verdict) => verdict === 'pass'), verdicts.join());

const accent = page.getByLabel('--surface in light');
await accent.fill('#fefefe');
await accent.press('Enter');
await page.waitForTimeout(300);
let text = await bufferText();
check('a token change is written into the file', text.includes('--surface: #fefefe;'));
check('comments and order are untouched', text.includes('--on-surface: #14171a; /* body text */') && text.startsWith('/* Brand tokens. */'));
await page.keyboard.press('Control+Shift+F');
await page.evaluate(() => window.__tesseraWorkbench.run('view.theme'));
await page.waitForSelector('#theme-panel .theme-table');
await page.click('.theme-undo');
await page.waitForTimeout(300);
text = await bufferText();
check('Undo token edit reverts it', text === THEME);

await page.evaluate(() => window.__tesseraWorkbench.run('view.theme'));
const onSurface = page.getByLabel('--on-surface in light');
await onSurface.fill('#cccccc');
await onSurface.press('Enter');
await page.waitForTimeout(300);
const failing = await page.$$eval('.theme-pair[data-pass="false"] .theme-pair-verdict', (cells) => cells.map((cell) => cell.textContent));
check('a low-contrast value reports fail as text', failing.length > 0 && failing.every((verdict) => verdict === 'fail'), failing.join());

const before = await bufferText();
await page.evaluate(() => window.__tesseraWorkbench.run('view.theme'));
const bad = page.getByLabel('--accent in dark');
await bad.fill('not a color');
await bad.press('Enter');
await page.waitForTimeout(300);
check('a non-color is refused with a sentence', (await page.locator('#theme-panel .theme-status').innerText()).includes('is a color'));
check('  and the file is unchanged', (await bufferText()) === before);

check('no page errors', errors.length === 0, errors.join(' | '));
await context.close();
await browser.close();
server.close();
console.log(failures === 0 ? '\nworkbench_theme: all checks passed' : '\nworkbench_theme: ' + failures + ' failed');
process.exit(failures === 0 ? 0 : 1);
