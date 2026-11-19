#!/usr/bin/env node
/**
 * Drive the assembled studio page in a real browser and check what it does.
 *
 * **Why this exists.** Every preview lane before it ended with "browser/WebGPU verification not
 * performed", because the repository had no way to open the page. That gap hid a real defect: the
 * canvas view lost its WebGPU device on every Run after the first (#430), which no unit test, parity
 * gate or static page check could see. Headless Chromium does provide a WebGPU device on this machine,
 * so the gap is closable.
 *
 * **Developer-run, not a CI gate.** It needs a browser and a WebGPU-capable driver, which no lane has
 * today; wiring one is an owner decision (the same one the P3 pixel-parity issue waits on). It is also
 * *not* a pixel golden: the software adapter proves the page acquires a device, hands it to the module
 * and draws, not that the pixels match lavapipe.
 *
 * Usage:
 *   node scripts/ci/preview_browser_verify.mjs --site build/pages [--keep] [--shot out.png]
 *
 * The site must be the assembled artifact (see `.github/workflows/pages.yml`'s assemble step): the
 * built front end plus the three Wasm modules. It is served over HTTP because a Wasm module cannot be
 * fetched from `file://`.
 */

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';

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

await page.goto(`${base}/index.html`, { waitUntil: 'load' });
await page.waitForTimeout(1200);

const status = async () =>
  page.evaluate(() => document.querySelector('#canvas-status')?.textContent ?? null);

// 1. The browser under test really has WebGPU, otherwise every later check is meaningless.
const hasGpu = await page.evaluate(async () => {
  if (!navigator.gpu) return false;
  const adapter = await navigator.gpu.requestAdapter();
  return adapter ? Boolean(await adapter.requestDevice()) : false;
});
check('WebGPU device is available', hasGpu);

// 2. The diagnostics module compiles the sample, **with no click**: since issue #444 the first load
//    applies itself once the module is ready, so the pane leaving "not run yet" is the acceptance
//    sentence for the automatic first apply. Waiting rather than clicking is the point.
await page.waitForFunction(
  () => {
    const text = document.querySelector('#diag-status')?.textContent ?? '';
    return text !== '' && text !== 'not run yet';
  },
  undefined,
  { timeout: 10000 },
);
check('Diagnostics applies the sample without a click',
  (await page.evaluate(() => document.querySelector('#diag-status')?.textContent ?? '')) !== 'not run yet');

// 3. The scene module evaluates.
await page.locator('#tab-scene').click();
await page.waitForTimeout(900);
check('Scene view opens', (await page.evaluate(() => document.body.innerText)).length > 0);

// 4. The canvas draws on its first open, also without a click, and keeps drawing. The *Restart app*
//    click below is the #430 regression: the module used to be re-instantiated per Run, so the new
//    instance had no device and the frame was replaced by "no WebGPU device: call
//    tessera_webgpu_set_device first".
await page.locator('#tab-canvas').click();
await page.waitForTimeout(2200);
check('Canvas draws a frame on its first open', /draw call/.test((await status()) ?? ''), await status());

// The viewport presets are Device > Viewport menu items now (issue #557); drive the command they run.
await page.evaluate(() => window.__tesseraWorkbench.run('device.setViewport', { value: '390x844' }));
await page.waitForTimeout(1600);
check('Canvas survives a viewport change', /draw call/.test((await status()) ?? ''), await status());

await page.locator('#restart-app').click();
await page.waitForTimeout(2200);
check('Canvas survives a restart', /draw call/.test((await status()) ?? ''), await status());

// 5. Every fold posture still draws (issue #408 feeds the fold through the app's safe-area path).
// The posture is the Device > Fold Posture menu now (issue #557): a command that must exist and be runnable.
const foldCommand = await page.evaluate(() => window.__tesseraWorkbench.describe('device.setFold', { value: 'book' }));
check('the fold-posture command is present', foldCommand !== null && foldCommand.runnable === true);
if (foldCommand !== null) {
  for (const posture of ['book', 'tabletop', 'flat']) {
    await page.evaluate((value) => window.__tesseraWorkbench.run('device.setFold', { value }), posture);
    await page.waitForTimeout(1300);
    check(`Canvas draws in the ${posture} posture`, /draw call/.test((await status()) ?? ''),
      await status());
  }
}

// 6. Text paints: the font loaded, so a label is a glyph run rather than a box.
check('The preview font loaded', !/font unavailable/i.test((await status()) ?? ''));

// 7. The Share feature is gone.
check('No Share control in the chrome', (await page.locator('#share').count()) === 0);

check('No console errors', consoleErrors.length === 0, consoleErrors.slice(0, 3).join(' | ') || 'none');

if (shot) {
  await page.screenshot({ path: shot });
  console.log(`screenshot: ${shot}`);
}

console.log(`\n${failures === 0 ? 'all browser checks passed' : `${failures} check(s) failed`}`);

if (!process.argv.includes('--keep')) {
  await browser.close();
  server.close();
}
process.exit(failures === 0 ? 0 : 1);
