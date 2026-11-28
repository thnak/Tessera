// SPDX-License-Identifier: MIT
//
// The shared plumbing of the developer-run studio browser scripts (`preview_browser_layout.mjs`,
// `preview_browser_settings.mjs`): a static server for the built site, Playwright resolution, the check
// reporter, and a page opener that records console errors. Playwright is not a repository dependency: install it
// anywhere and set PLAYWRIGHT_DIR to that directory, or run from a directory whose node_modules has it.

import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

import { writeZip } from '../../designer/web/studio-zip.js';

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.wasm': 'application/wasm',
  '.ttf': 'font/ttf', '.txt': 'text/plain; charset=utf-8',
};

export const argument = (name, fallback) => {
  const index = process.argv.indexOf(name);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
};

export async function loadChromium() {
  let mod;
  try {
    mod = await import('playwright');
  } catch {
    const dir = process.env.PLAYWRIGHT_DIR;
    if (!dir) {
      console.error('playwright is not installed. This script is developer-run on purpose (no repository dependency):\n  npm install playwright && npx playwright install chromium\nthen set PLAYWRIGHT_DIR to that directory, or run from it.');
      process.exit(2);
    }
    mod = await import(pathToFileURL(createRequire(join(resolve(dir), 'noop.js')).resolve('playwright')).href);
  }
  return mod.chromium ?? mod.default.chromium;
}

/** Serves `site` on a free port; `close()` stops it. */
export async function serve(site) {
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
  await new Promise((done) => server.listen(0, '127.0.0.1', done));
  return { base: `http://127.0.0.1:${server.address().port}`, close: () => server.close() };
}

export function reporter() {
  const state = { failures: 0 };
  const check = (name, ok, detail = '') => {
    console.log(`${ok ? 'ok   ' : 'FAIL '} ${name}${detail ? '  -- ' + detail : ''}`);
    if (!ok) state.failures += 1;
  };
  const skip = (name, why) => console.log(`skip  ${name}  -- ${why}`);
  return { state, check, skip };
}

const encoder = new TextEncoder();
export const zipOf = (files) => Buffer.from(writeZip(Object.entries(files).map(([path, text]) => ({ path, bytes: encoder.encode(text) }))));
export const PROJECT = {
  'tessera.json': '{"schemaVersion":1,"app":{"name":"p","entry":"ui/Home.tsx","namespace":"p"}}',
  'ui/Home.tsx': 'export function Home(): Node {\n  return <View><Text>hi</Text></View>;\n}\n',
  'ui/Other.tsx': 'export function Other(): Node {\n  return <View><Text>other</Text></View>;\n}\n',
};

export const boxOf = (page, selector) =>
  page.evaluate((selector_) => {
    const node = document.querySelector(selector_);
    if (!node || !node.isConnected) return null;
    const rect = node.getBoundingClientRect();
    return { x: Math.round(rect.x), y: Math.round(rect.y), w: Math.round(rect.width), h: Math.round(rect.height) };
  }, selector);

/** Opens a page. `init` runs before the page's scripts; `storage` seeds localStorage. Console errors are collected. */
export async function openPage(browser, base, width, height, { init, storage, path = '/index.html' } = {}) {
  const context = await browser.newContext({ viewport: { width, height } });
  if (init) await context.addInitScript(init);
  // The mutation-check hook: STUDIO_BROWSER_MUTATION names a script that deliberately breaks one behavior, to prove the
  // assertion for it fails. It runs after `init`, so it can also add the console error a regressed fallback would log.
  const mutation = process.env.STUDIO_BROWSER_MUTATION;
  if (storage) await context.addInitScript((entries) => { for (const [k, v] of Object.entries(entries)) localStorage.setItem(k, v); }, storage);
  if (mutation) await context.addInitScript({ path: mutation });
  const page = await context.newPage();
  const errors = [];
  page.on('console', (message) => { if (message.type() === 'error' && !/404/.test(message.text())) errors.push(message.text().slice(0, 200)); });
  page.on('pageerror', (error) => errors.push(`pageerror: ${String(error).slice(0, 300)}`));
  await page.goto(`${base}${path}`, { waitUntil: 'load' });
  await page.waitForSelector('.flexlayout__layout', { timeout: 15000 });
  await page.waitForTimeout(800);
  return { page, errors, context };
}

/** Opens `files` as a project through the zip chooser, so a file is open and editable. */
export async function openProject(page, files = PROJECT) {
  await page.locator('input[type=file]').first().setInputFiles({ name: 'p.zip', mimeType: 'application/zip', buffer: zipOf(files) });
  await page.waitForSelector('#project-name');
  await page.waitForTimeout(800);
}
