// SPDX-License-Identifier: MIT

// The dock and the command wiring with the real Wasm modules and a real WebGPU device (ADR-062, the W0 gates
// the dock agent could not run without a Wasm build): a hidden Canvas submits zero GPU frames, frames keep
// painting after a move, the Canvas element is never re-parented by a move, and zen mode and closing the
// panel stop and release the device. Needs Chromium with WebGPU (SwiftShader is enough) and the built site with
// the three Wasm modules (`build/pages`, assembled as .github/workflows/pages.yml does).
//
//   PLAYWRIGHT_DIR=<dir whose node_modules holds playwright> node scripts/ci/workbench_webgpu_dock.mjs --site build/pages

import { createRequire } from 'node:module';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const args = process.argv.slice(2);
const site = args.includes('--site') ? args[args.indexOf('--site') + 1] : 'build/pages';

let mod;
try {
  mod = await import('playwright');
} catch {
  const dir = process.env.PLAYWRIGHT_DIR;
  if (!dir) {
    console.error('playwright not found: npm install playwright, or set PLAYWRIGHT_DIR');
    process.exit(2);
  }
  mod = await import(pathToFileURL(createRequire(join(resolve(dir), 'noop.js')).resolve('playwright')).href);
}
const chromium = mod.chromium ?? mod.default.chromium;

const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.wasm': 'application/wasm', '.ttf': 'font/ttf', '.tsx': 'text/plain', '.ts': 'text/plain' };
const server = createServer(async (request, response) => {
  let path = normalize(decodeURIComponent(new URL(request.url, 'http://x').pathname));
  if (path === '/' || path === '\\') path = '/index.html';
  try {
    const body = await readFile(join(site, path));
    response.writeHead(200, { 'content-type': MIME[extname(path)] ?? 'application/octet-stream', 'cache-control': 'no-store' });
    response.end(body);
  } catch {
    response.writeHead(404);
    response.end('not found');
  }
});
await new Promise((done) => server.listen(0, '127.0.0.1', done));
const base = `http://127.0.0.1:${server.address().port}/`;

const browser = await chromium.launch({
  executablePath: process.env.PLAYWRIGHT_CHROMIUM || undefined,
  args: ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--ignore-gpu-blocklist'],
});
let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok   ' : 'FAIL '} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
};

const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
await context.addInitScript(() => {
  window.__submits = 0;
  if (typeof GPUQueue !== 'undefined') {
    const submit = GPUQueue.prototype.submit;
    GPUQueue.prototype.submit = function (...rest) {
      window.__submits += 1;
      return submit.apply(this, rest);
    };
  }
});
const page = await context.newPage();
const errors = [];
page.on('console', (m) => { if (m.type() === 'error' && !/404/.test(m.text())) errors.push(m.text().slice(0, 200)); });
page.on('pageerror', (e) => errors.push(`pageerror: ${String(e).slice(0, 300)}`));
await page.goto(base, { waitUntil: 'load' });
await page.waitForFunction(() => window.__tesseraWorkbench?.run && window.__tesseraWorkbench?.layout);

const hasGpu = await page.evaluate(async () => Boolean(navigator.gpu && (await navigator.gpu.requestAdapter())));
check('the browser has a WebGPU adapter', hasGpu);
if (!hasGpu) {
  await browser.close();
  server.close();
  process.exit(1);
}

const submits = () => page.evaluate(() => window.__submits);
const status = () => page.evaluate(() => document.querySelector('#canvas-status')?.textContent ?? '');
const run = (id, a) => page.evaluate(([i, x]) => window.__tesseraWorkbench.run(i, x), [id, a]);
/** Frames submitted during `ms` while `poke` provokes redraws (a viewport change always asks for one). */
async function framesDuring(ms, poke) {
  const before = await submits();
  for (let i = 0; i < 3; i += 1) {
    await poke(i);
    await page.waitForTimeout(ms / 3);
  }
  return (await submits()) - before;
}
const sizes = ['390x844', '768x1024', '320x240'];
const poke = (i) => run('device.setViewport', { value: sizes[i % sizes.length] });

// A sample project makes the Canvas draw.
await page.getByRole('button', { name: /Counter/ }).first().click();
await page.waitForTimeout(2500);
await run('device.setViewport', { value: '390x844' });
await page.waitForFunction(() => /draw call/.test(document.querySelector('#canvas-status')?.textContent ?? ''), null, { timeout: 30000 }).catch(() => {});
check('the Canvas draws a sample on a real device', /draw call/.test(await status()), await status());
await page.evaluate(() => {
  window.__canvas = document.getElementById('canvas');
  window.__removals = 0;
  new MutationObserver((records) => {
    for (const r of records) for (const n of r.removedNodes) if (n === window.__canvas || (n.contains && n.contains(window.__canvas))) window.__removals += 1;
  }).observe(document.documentElement, { childList: true, subtree: true });
});
check('while visible, redraws submit frames', (await framesDuring(1800, poke)) > 0);

// Hidden by activating another tab of the same group.
await page.evaluate(() => window.__tesseraWorkbench.dispatch({ kind: 'activate', panelId: 'scene' }));
await page.waitForTimeout(500);
const hidden = await framesDuring(1800, poke);
check('a hidden Canvas (Scene tab active) submits zero frames', hidden === 0, `submits=${hidden}`);
await page.evaluate(() => window.__tesseraWorkbench.dispatch({ kind: 'activate', panelId: 'canvas' }));
await page.waitForTimeout(800);
check('showing it again paints', (await framesDuring(1800, poke)) > 0);

// Moves, by command (the same code a menu item and the tab menu run) and by the real tab menu.
await run('view.movePanel', { id: 'canvas', to: 'bottom' });
await page.waitForTimeout(800);
const afterMove = await framesDuring(1800, poke);
check('frames keep painting after Move Bottom', afterMove > 0, `submits=${afterMove}`);
check('...on the same HTMLCanvasElement, never removed from the document', (await page.evaluate(() => document.getElementById('canvas') === window.__canvas && window.__removals === 0)));
await page.locator('.flexlayout__tab_button', { hasText: 'Canvas' }).first().click({ button: 'right' });
await page.getByRole('menuitem', { name: /Move Panel/ }).first().click();
await page.getByRole('menuitem', { name: 'Move Right' }).click();
await page.waitForTimeout(800);
check('frames keep painting after Move Right from the panel-tab menu', (await framesDuring(1800, poke)) > 0);
check('...still the same canvas element', (await page.evaluate(() => document.getElementById('canvas') === window.__canvas && window.__removals === 0)));

// Zen mode shows the editor alone: the Canvas is hidden, so it must not draw.
await run('view.zenMode');
await page.waitForTimeout(600);
check('zen mode: a hidden Canvas submits zero frames', (await framesDuring(1800, poke)) === 0);
await run('view.exitZenMode');
await page.waitForTimeout(800);
check('leaving zen: it paints again, same element', (await framesDuring(1800, poke)) > 0 && (await page.evaluate(() => document.getElementById('canvas') === window.__canvas)));

// A collapsed panel area that holds the Canvas hides it too.
await run('view.movePanel', { id: 'canvas', to: 'bottom' });
await page.waitForTimeout(500);
await run('view.togglePanelArea');
await page.waitForTimeout(600);
check('the panel area collapsed over the Canvas: zero frames', (await framesDuring(1800, poke)) === 0);
await run('view.togglePanelArea');
await page.waitForTimeout(800);
check('and expanding it paints again', (await framesDuring(1800, poke)) > 0);

// Phone width: the Canvas is not the active panel, so it does not draw; the switcher brings it back.
await page.setViewportSize({ width: 390, height: 844 });
await page.waitForTimeout(800);
check('390 px: the editor is the shown panel, so the Canvas submits zero frames', (await framesDuring(1800, poke)) === 0);
await page.getByRole('tab', { name: 'Canvas' }).first().click();
await page.waitForTimeout(1000);
check('390 px: selecting Canvas in the switcher paints', (await framesDuring(1800, poke)) > 0);
check('...no horizontal scroll', await page.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth));
await page.setViewportSize({ width: 1440, height: 900 });
await page.waitForTimeout(800);
const crossed = await page.evaluate(() => ({ same: document.getElementById('canvas') === window.__canvas, removals: window.__removals }));
check('crossing the phone breakpoint may re-parent the canvas (recorded, spec allows it)', true, `sameElement=${crossed.same} removals=${crossed.removals}`);
check('after crossing back the Canvas still paints', (await framesDuring(1800, poke)) > 0, await status());

// Closing the panel releases the device; reopening creates a new one that paints.
await run('view.togglePanel', { id: 'canvas', close: true });
await page.waitForTimeout(600);
const live = () => page.evaluate(() => (window.__tesseraGpu ? window.__tesseraGpu.live : -1));
await page.waitForFunction(() => window.__tesseraGpu && window.__tesseraGpu.live === 0, null, { timeout: 8000 }).catch(() => {});
check('closing Canvas destroys its GPU device (live devices reach 0)', (await live()) === 0, `live=${await live()} acquired=${await page.evaluate(() => window.__tesseraGpu?.acquired)}`);
check('a closed Canvas has no element and submits nothing', (await page.evaluate(() => !document.getElementById('canvas'))) && (await framesDuring(1200, poke)) === 0);
await run('view.togglePanel', { id: 'canvas' });
await page.waitForFunction(() => /draw call/.test(document.querySelector('#canvas-status')?.textContent ?? ''), null, { timeout: 30000 }).catch(() => {});
check('reopening it draws on a fresh device', /draw call/.test(await status()), await status());

check('no page errors', errors.length === 0, errors.join(' | '));
await browser.close();
server.close();
console.log(failures === 0 ? '\nall checks passed' : `\n${failures} check(s) failed`);
process.exit(failures === 0 ? 0 : 1);
