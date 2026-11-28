// SPDX-License-Identifier: MIT
//
// Browser check of the editor tab strip (#562): a single click on a tree row opens a replaceable italic preview
// tab, a second single click replaces it, a double click or an edit pins it, the breadcrumbs show the path, and
// the dirty state and the Canvas element are unaffected.
//
//   node scripts/ci/preview_browser_editor_tabs.mjs [site-dir]
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { chromium } from 'playwright';
import { ENTRY, SHEET, ensureProject } from './studio_project.mjs';

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8', '.wasm': 'application/wasm', '.ttf': 'font/ttf', '.txt': 'text/plain; charset=utf-8' };
const site = process.argv[2] ?? 'build/pages';
const server = createServer(async (request, response) => {
  const url = new URL(request.url, 'http://127.0.0.1');
  let path = normalize(decodeURIComponent(url.pathname));
  if (path === '/' || path === '\\') path = '/index.html';
  try {
    const body = await readFile(join(site, path));
    response.writeHead(200, { 'content-type': MIME[extname(path).toLowerCase()] ?? 'application/octet-stream', 'cache-control': 'no-store' });
    response.end(body);
  } catch {
    response.writeHead(404);
    response.end();
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const browser = await chromium.launch({ args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader'] });
const page = await (await browser.newContext()).newPage();
const errors = [];
page.on('pageerror', (error) => errors.push(String(error)));
await page.goto(`http://127.0.0.1:${server.address().port}`, { waitUntil: 'load' });
await ensureProject(page);
await page.waitForFunction(() => (window.__tesseraEditor?.text() ?? '').length > 0);

let failures = 0;
const check = (name, ok, detail) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
};
const row = (path) => page.locator(`#project-tree [role="treeitem"][data-path="${path}"]`);
const tabs = () => page.locator('.editor-tab-select').evaluateAll((nodes) => nodes.map((n) => n.title));
const previews = () => page.locator('.editor-tab.is-preview .editor-tab-select').evaluateAll((nodes) => nodes.map((n) => n.title));
const crumbs = () => page.locator('#editor-breadcrumbs li').allInnerTexts();
const canvasId = () => page.evaluate(() => { const c = document.getElementById('canvas'); if (c && !c.__probe) c.__probe = Math.random(); return c ? c.__probe : null; });

const before = await canvasId();
// The seed opens with the entry as a pinned tab (a project opens its entry as an ordinary tab).
check('the entry tab is open and not a preview', (await tabs()).includes(ENTRY) && !(await previews()).includes(ENTRY), JSON.stringify(await tabs()));
check('breadcrumbs show the path of the active file', (await crumbs()).join('>') === ENTRY.split('/').join('>'), (await crumbs()).join('>'));

await row(SHEET).click();
check('a single click opens a preview tab beside the pinned one', JSON.stringify(await previews()) === JSON.stringify([SHEET]) && (await tabs()).length === 2, JSON.stringify(await tabs()));
check('the preview tab is italic', await page.locator('.editor-tab.is-preview .editor-tab-name').evaluate((n) => getComputedStyle(n).fontStyle === 'italic'));
check('breadcrumbs follow the active tab', (await crumbs()).join('>') === SHEET.split('/').join('>'));

await row('tessera.json').click();
check('the next single click replaces the preview (still two tabs)', (await tabs()).length === 2 && JSON.stringify(await previews()) === JSON.stringify(['tessera.json']), JSON.stringify(await tabs()));

await page.locator('.editor-tab.is-preview').dblclick();
check('a double click on the tab pins it', (await previews()).length === 0 && (await tabs()).length === 2);
await row(SHEET).click();
check('after pinning, the next single click opens a third tab', (await tabs()).length === 3 && (await previews()).length === 1, JSON.stringify(await tabs()));

await page.evaluate((next) => window.__tesseraEditor.edit(next), '/* edited */\n' + (await page.evaluate(() => window.__tesseraEditor.text())));
await page.waitForTimeout(200);
check('an edit pins the preview and marks it unsaved', (await previews()).length === 0 && (await page.locator('.editor-tab-dirty').count()) >= 1);
await row('ui/App.tsx').click();
await row(SHEET).click();
check('a pinned dirty tab survives later previews', (await tabs()).includes(SHEET));

await row(ENTRY).dblclick();
check('a double click on a tree row pins its tab', (await previews()).length === 0);
check('the Canvas element was never replaced', before === (await canvasId()));
check('no page errors', errors.length === 0, errors.join(' | '));
await browser.close();
server.close();
process.exit(failures > 0 ? 1 : 0);
