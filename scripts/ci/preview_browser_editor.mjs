// SPDX-License-Identifier: MIT
//
// Browser check of the studio's code editor (issue #517, CodeMirror 6): the tokens are coloured in both
// themes, a compiler diagnostic is underlined at its span, undo survives a tab switch, a user edit reaches
// the page's buffer, and Mod-S is the editor's own save key.
//
//   node scripts/ci/preview_browser_editor.mjs [site-dir]
//
// Not a repository test: run it from a directory whose node_modules has playwright. The page's test seam
// `window.__tesseraEditor` (`text()`, `edit(text)`) is how it reads and types, because a CodeMirror surface
// has no `value` to set.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { chromium } from 'playwright';
import { ENTRY, SHEET, ensureProject, openPanel, selectFile, tab } from './studio_project.mjs';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.ttf': 'font/ttf',
  '.txt': 'text/plain; charset=utf-8',
};
const site = process.argv[2] ?? 'build/pages';
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
    response.writeHead(404);
    response.end();
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch({
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader'],
});
const page = await (await browser.newContext()).newPage();
const consoleErrors = [];
page.on('console', (message) => {
  if (message.type() === 'error') consoleErrors.push(message.text());
});
page.on('pageerror', (error) => consoleErrors.push(`pageerror: ${String(error)}`));
await page.goto(base, { waitUntil: 'load' });
// The studio no longer boots with a project open (issue #391), so the probes open one through the
// page's own Open Zip route first (issue #849).
await ensureProject(page);
await page.waitForFunction(() => (window.__tesseraEditor?.text() ?? '').length > 0);

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
}

const text = () => page.evaluate(() => window.__tesseraEditor.text());
const type = (next) => page.evaluate((value) => window.__tesseraEditor.edit(value), next);

/** The colour of the first `.cm-line` span whose text is exactly `word`, or null. */
const colourOf = (word) =>
  page.evaluate((needle) => {
    for (const node of document.querySelectorAll('#editor-host .cm-line span')) {
      if (node.textContent === needle) return getComputedStyle(node).color;
    }
    return null;
  }, word);
const plain = () =>
  page.evaluate(() => getComputedStyle(document.querySelector('#editor-host .cm-content')).color);

// --- 1. token colours, in both themes ----------------------------------------------------------------
await type(`// a comment
export function Screen(): Node {
  const label = "hello";
  return <View className="page"><Text>{label}</Text></View>;
}
`);
await page.waitForTimeout(300);
for (const scheme of ['light', 'dark']) {
  await page.emulateMedia({ colorScheme: scheme });
  await page.waitForTimeout(150);
  const base_ = await plain();
  const keyword = await colourOf('export');
  const string = await colourOf('"hello"');
  const comment = await colourOf('// a comment');
  check(`${scheme}: a keyword is coloured, not the plain text colour`, keyword !== null && keyword !== base_, `${keyword} vs ${base_}`);
  check(`${scheme}: a string is coloured and differs from a keyword`, string !== null && string !== base_ && string !== keyword, `${string}`);
  check(`${scheme}: a comment is coloured and differs from a string`, comment !== null && comment !== base_ && comment !== string, `${comment}`);
}
await page.emulateMedia({ colorScheme: 'light' });

// --- 2. CSS is highlighted too -----------------------------------------------------------------------
await selectFile(page, SHEET);
await page.waitForFunction(() => window.__tesseraEditor.path.endsWith('.css'));
await type('.page { background-color: #123456; width: 320px; }\n');
await page.waitForTimeout(200);
const property = await colourOf('background-color');
check('a CSS property name is coloured', property !== null && property !== (await plain()), `${property}`);

// --- 3. undo survives a tab switch -------------------------------------------------------------------
await selectFile(page, ENTRY);
await page.waitForFunction((entry) => window.__tesseraEditor.path === entry, ENTRY);
const original = await text();
await type(`${original}// typed after load\n`);
await selectFile(page, SHEET);
await page.waitForFunction((sheet) => window.__tesseraEditor.path === sheet, SHEET);
await selectFile(page, ENTRY);
await page.waitForFunction((entry) => window.__tesseraEditor.path === entry, ENTRY);
check('the typed text is back after switching tabs', (await text()).includes('typed after load'));
await page.locator('#source').click();
await page.keyboard.press('Control+z');
check('undo still works after a tab switch', !(await text()).includes('typed after load'), (await text()).slice(-40));

// --- 4. a compiler diagnostic is underlined at its span ----------------------------------------------
await openPanel(page, 'diagnostics');
await tab(page, 'diagnostics');
await type(`export function Screen(): Node {
  const x = ;
  return <View/>;
}
`);
await page.waitForFunction(() => document.querySelectorAll('#editor-host .cm-diag').length > 0, null, { timeout: 8000 }).catch(() => undefined);
const underlined = await page.locator('#editor-host .cm-diag').count();
check('a diagnostic is underlined in the editor', underlined > 0, `${underlined} mark(s)`);
if (underlined > 0) {
  const decoration = await page.evaluate(() => {
    const style = getComputedStyle(document.querySelector('#editor-host .cm-diag'));
    return `${style.textDecorationLine} ${style.textDecorationStyle}`;
  });
  check('the underline is wavy', /wavy/.test(decoration), decoration);
}
await type(`export function Screen(): Node {
  return <View/>;
}
`);
await page.waitForFunction(() => document.querySelectorAll('#editor-host .cm-diag').length === 0, null, { timeout: 8000 }).catch(() => undefined);
check('the underline clears when the source is fixed', (await page.locator('#editor-host .cm-diag').count()) === 0);

// --- 5. a user edit reaches the page; Mod-S is the editor's key --------------------------------------
await type(`${await text()}// dirty\n`);
await page.waitForTimeout(200);
check('a typed edit marks the file unsaved', (await page.locator('.editor-tab-dirty').count()) >= 1);
await page.locator('#source').click();
const downloaded = page.waitForEvent('dialog', { timeout: 1500 }).catch(() => null);
await page.keyboard.press('Control+s');
check('Ctrl+S in the editor is handled (no browser save dialog)', (await downloaded) === null);
const note = ((await page.locator('#project-note').textContent()) ?? '').trim();
check('Ctrl+S says what it did (no folder is open)', /No folder is open/.test(note), note);

check(
  'No console errors',
  consoleErrors.filter((entry) => !/Failed to load resource/.test(entry)).length === 0,
  consoleErrors.slice(0, 3).join(' | ') || 'none',
);
console.log(`\n${failures === 0 ? 'all editor checks passed' : `${failures} check(s) failed`}`);
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
