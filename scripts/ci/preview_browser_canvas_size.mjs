// SPDX-License-Identifier: MIT
//
// CI browser acceptance for #598, the canvas backing store that never followed the viewport:
//
//   node scripts/ci/preview_browser_canvas_size.mjs [site-dir] [--shot out.png]
//
// **#598 — "the canvas stays 320x240 after a viewport change".** The report suspected
// `CanvasPane.tsx`'s React-owned `width={320} height={240}` fighting the session's own
// `sizeToDevicePixels` writes. On this tree that does not reproduce: with the attributes restored the
// checks below still pass, because the session writes after React's commit and React never re-renders
// the leaf. The issue says so itself ("strong suspect, not proven"). This gate pins the behaviour the
// issue is about - the store follows the viewport and agrees with the CSS box and the DPR - so it
// fails if an author ever does win.
//
// **Why this needs a browser.** #598 is about which author wins for `canvas.width` after React commits
// and after the session's own writes: a Node gate can test `sizeToDevicePixels`' arithmetic
// (`preview_viewport_limits.mjs`, `preview_canvas_zoom.mjs`) but not who writes last.
//
// **The paint half of canvasing (#599) is not here.** A presented WebGPU frame is not composited by
// headless Chromium, so reading it back (`drawImage` into a 2D canvas, or `screenshot()`) returns
// zeros however correct the paint is; `canvas-session.ts` records the same limitation for the page's
// own screenshot path. The DPR-2 paint is therefore proven where a reference can be pinned: the
// `dpr` case of `scripts/ci/scene_frame_parity.py`, on lavapipe, where the far corner of a doubled
// target must be the box's colour.

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { chromium } from 'playwright';
import { writeZip } from '../../designer/web/studio-zip.js';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.ttf': 'font/ttf',
  '.txt': 'text/plain; charset=utf-8',
  '.svg': 'image/svg+xml',
};

const args = process.argv.slice(2);
const shotIndex = args.indexOf('--shot');
const shot = shotIndex >= 0 ? args[shotIndex + 1] : null;
const site = resolve(args.find((value, index) => index === 0 && value !== '--shot') ?? 'build/pages');

const GPU_ARGS = ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader', '--no-sandbox'];

// One full-bleed box: every pixel of the frame is the same colour, so "did the paint cover the whole
// surface" is a question about two samples (a corner and the centre), not about a layout reference.
// The colour is deliberately not the clear colour (#1e2433) nor white, so a blank or cleared frame
// cannot be mistaken for a painted one.
const PROJECT = {
  'tessera.json': `{
  "schemaVersion": 1,
  "app": { "name": "canvas-size-check", "entry": "ui/App.tsx", "namespace": "canvas_size_check" }
}
`,
  'ui/App.tsx': `export function App() {
  return <View style={{ width: "100%", height: "100%", backgroundColor: "#e0115f" }} />;
}
`,
};

const encoder = new TextEncoder();
const projectZip = Buffer.from(
  writeZip(Object.entries(PROJECT).map(([path, text]) => ({ path, bytes: encoder.encode(text) }))),
);

function startServer() {
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
  return new Promise((done) => {
    server.listen(0, '127.0.0.1', () => done({ server, base: `http://127.0.0.1:${server.address().port}` }));
  });
}

let failures = 0;
function check(name, ok, detail) {
  // The detail is printed on a failure: on a pass it is the observation, which is noise unless the
  // number is the point, and `--shot` covers that.
  process.stdout.write(`${ok ? 'ok  ' : 'FAIL'} ${name}${!ok && detail ? `  -- ${detail}` : ''}\n`);
  if (!ok) {
    failures += 1;
  }
}

async function settles(expression, page, timeout = 20000, argument = undefined) {
  try {
    await page.waitForFunction(expression, argument, { timeout });
    return true;
  } catch {
    return false;
  }
}

/** The canvas' geometry and the pane's state, as the page actually has them. */
function readCanvas(page) {
  return page.evaluate(() => {
    const canvas = document.getElementById('canvas');
    return {
      width: canvas.width,
      height: canvas.height,
      cssWidth: canvas.style.width,
      cssHeight: canvas.style.height,
      unsupported: !document.getElementById('unsupported').hidden,
      cause: document.getElementById('unsupported').dataset.cause ?? '',
      body: document.getElementById('unsupported-body').textContent,
      status: document.getElementById('canvas-status').textContent ?? '',
      dpr: (document.getElementById('stat-dpr')?.textContent ?? '').trim(),
    };
  });
}

/**
 * Opens a Device submenu and clicks the item whose label matches `label`.
 *
 * The five device selects became menu items with the workbench (#586), and their DOM ids are derived
 * from the ROW INDEX (`mi-device_device_viewport2_device_setViewport0_3`), so an id is not a stable
 * handle. The label is what a person clicks, so that is what this drives: `device.setViewport` ->
 * `viewport.set` -> `tessera_canvas_set_viewport` and the session's `onEnvironmentChange`, the path
 * both bugs are on.
 */
async function chooseFromDeviceMenu(page, submenu, label) {
  await page.click('#menubar-device');
  await settles(() => document.getElementById('menu-device') !== null, page, 5000);
  // The submenu's own row; hovering opens it (the menu opens submenus on hover and on click).
  const row = page.locator('#menu-device [aria-haspopup="menu"]', { hasText: submenu });
  await row.hover();
  const item = page.locator('#menu-device [role="menuitemradio"], #menu-device [role="menuitem"]', {
    hasText: label,
  });
  await item.first().click();
}

/** Waits until the frame counter has moved past `before`, i.e. a frame was actually painted. */
async function nextFrame(page, before) {
  return settles(
    (previous) =>
      (document.getElementById('stat-frame')?.textContent ?? "").trim() !== "" &&
      (document.getElementById('stat-frame')?.textContent ?? "").trim() !== previous,
    page,
    15000,
    before,
  );
}

/**
 * The pane's lifetime frame counter (`#stat-frame`), as a number, or -1 before the first frame.
 *
 * The cell reads `1536x2048 (#3, c1)`: the dimensions, the frame count, then the cascade generation.
 * The frame is the `#`-prefixed group, not the size, so a viewport change does not look like a frame
 * change on its own.
 */
async function frameCount(page) {
  const text = await page.evaluate(
    () => (document.getElementById('stat-frame')?.textContent ?? '').trim(),
  );
  const match = /#\s*(\d+)/.exec(text);
  return match ? Number(match[1]) : -1;
}

async function openCanvas(context, base) {
  const page = await context.newPage();
  const errors = [];
  page.on('console', (message) => {
    if (message.type() === 'error') errors.push(message.text());
  });
  page.on('pageerror', (error) => errors.push(`pageerror: ${String(error)}`));
  page.on('dialog', (dialog) => void dialog.accept());
  await page.goto(`${base}/index.html?view=canvas`, { waitUntil: 'load' });
  await page.locator('input[type="file"][accept*="zip"]').setInputFiles({
    name: 'canvas-size-check.zip',
    mimeType: 'application/zip',
    buffer: projectZip,
  });
  await settles(() => document.querySelector('#project-tree [data-path="ui/App.tsx"]') !== null, page);
  await settles(() => {
    const pane = document.getElementById('unsupported');
    const status = document.getElementById('canvas-status').textContent ?? '';
    return !pane.hidden || /draw call|not applied|fail|unavailable/i.test(status);
  }, page);
  return page;
}

const { server, base } = await startServer();
const browser = await chromium.launch({ args: GPU_ARGS });

try {
  // --- #598: the backing store follows the viewport ------------------------------------------------
  //
  // Run with no device: this half is about WHICH code sets `canvas.width`, and `sizeToDevicePixels`
  // runs before the module is ever asked for a frame. A browser without WebGPU therefore still proves
  // it, which is what makes this check portable to a runner that cannot keep a device.
  const sizingContext = await browser.newContext({ viewport: { width: 1400, height: 1000 } });
  const sizingPage = await openCanvas(sizingContext, base);

  const first = await readCanvas(sizingPage);
  // The default viewport IS 320x240, so the bug is not "the number is 320x240" - it is that the
  // number never moves again. The assertion is therefore the move, and the frame it cost.
  //
  // **This gate cannot distinguish the two authors, and that is recorded rather than fixed.** With
  // `width={320} height={240}` restored on the element (the pre-#598 markup), these checks still pass
  // on this tree, because the session's writes land after React's commit and React never
  // re-renders the leaf. #598's report is therefore not reproducible against `main` today, and the
  // attributes were left alone rather than removed on a theory. What this file pins is the property
  // the issue is about - the store follows the viewport, the box keeps the logical size, and the
  // two agree with the reported DPR - so a react author that DOES win later turns it red.
  const firstFrame = await frameCount(sizingPage);

  await chooseFromDeviceMenu(sizingPage, 'Viewport', '1280x800');
  const moved = await nextFrame(sizingPage, firstFrame);
  check('a viewport preset paints a frame (#598)', moved, `frame did not move past ${firstFrame}`);

  const resized = await readCanvas(sizingPage);
  check(
    'a viewport change resizes the backing store (#598)',
    resized.width !== first.width || resized.height !== first.height,
    `before ${first.width}x${first.height}, after ${resized.width}x${resized.height}`,
  );
  check(
    'the CSS box follows the viewport too (#598)',
    resized.cssWidth !== first.cssWidth || resized.cssHeight !== first.cssHeight,
    `style=${resized.cssWidth} x ${resized.cssHeight}`,
  );
  check(
    'the surface is large enough for the profile it was given (#598)',
    resized.width >= 1280 && resized.height >= 800,
    `${resized.width}x${resized.height} for a 1280x800 profile at DPR ${resized.dpr || '?'}`,
  );

  // The picker and the inspector map a pointer through the canvas' CSS box, so a box that no longer
  // matches the surface would put every outline in the wrong place. The rule is the picker's own
  // (`canvasMapping`), restated as an invariant here: CSS size * DPR == backing store.
  const dpr = Number(resized.dpr) || 1;
  const expectedWidth = Math.round(Number.parseFloat(resized.cssWidth) * dpr);
  check(
    'the backing store is the CSS box times the DPR (#598, the picker coordinate rule)',
    Math.abs(resized.width - expectedWidth) <= 1,
    `css ${resized.cssWidth} x DPR ${dpr} = ${expectedWidth}, canvas.width = ${resized.width}`,
  );
  await sizingContext.close();

  // --- DPR 2 geometry: the store and the box both double ------------------------------------------
  //
  // The PAINT at DPR 2 is `scene_frame_parity.py`'s `dpr` case (lavapipe): a presented WebGPU frame
  // cannot be read back in headless Chromium, so the assertion here stops at the two numbers the
  // page owns - the backing store and the CSS box - which is where #598 and #599 both started.
  const scaledContext = await browser.newContext({ viewport: { width: 1400, height: 1000 } });
  const scaledPage = await openCanvas(scaledContext, base);

  const before768 = await frameCount(scaledPage);
  await chooseFromDeviceMenu(scaledPage, 'Viewport', '768x1024');
  await nextFrame(scaledPage, before768);
  const before2 = await frameCount(scaledPage);
  await chooseFromDeviceMenu(scaledPage, 'Pixel Ratio', '2');
  await nextFrame(scaledPage, before2);

  const doubled = await readCanvas(scaledPage);
  check(
    'a DPR change doubles the backing store (#598, #599)',
    doubled.width === 1536 && doubled.height === 2048,
    `canvas=${doubled.width}x${doubled.height}, wanted 1536x2048 for 768x1024 at DPR 2 (reported DPR ${doubled.dpr || '?'})`,
  );
  check(
    'the CSS box stays the logical viewport at DPR 2 (#598)',
    doubled.cssWidth === '768px' && doubled.cssHeight === '1024px',
    `style=${doubled.cssWidth} x ${doubled.cssHeight}`,
  );
  const doubledDpr = Number(doubled.dpr) || 1;
  check(
    'the picker coordinate rule holds at DPR 2 (#598)',
    doubledDpr === 2 &&
      Math.abs(doubled.width - Number.parseFloat(doubled.cssWidth) * doubledDpr) <= 1,
    `css ${doubled.cssWidth} x DPR ${doubledDpr} vs canvas.width ${doubled.width}`,
  );
  if (shot) {
    await scaledPage.screenshot({ path: shot, fullPage: false });
    process.stdout.write(`shot: ${shot}\n`);
  }
  await scaledContext.close();
} finally {
  await browser.close();
  server.close();
}

const verdict = failures > 0 ? `${failures} check(s) failed` : 'all checks passed';
process.stdout.write(`\ncanvas size: ${verdict}\n`);
process.exit(failures === 0 ? 0 : 1);
