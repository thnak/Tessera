// SPDX-License-Identifier: MIT
//
// CI browser acceptance for the fold-hinge and safe-area overlay (issue #433).
//
//   node scripts/ci/preview_browser_fold_overlay.mjs [site-dir] [--shot out.png]
//
// **What only a browser can show.** `preview_fold_overlay.mjs` gates the scaling arithmetic in Node:
// a report in, the boxes out. What that cannot see is whether the overlay is wired at all - the pane's
// toggle exists, the session asks the module, the boxes reach the DOM, and the export answers the
// geometry of the posture the toolbar set. The session is created before React commits the overlay
// element (the same ordering the resize grips hit), so a listener or a lookup that runs too early
// would leave the overlay silently empty while every Node check stayed green.
//
// **What this does NOT prove, and why.** The pixels the overlay draws are page DOM the browser
// composites over the canvas; the engine's frame is untouched. A presented WebGPU frame cannot be read
// back in headless Chromium (see `preview_browser_canvas_size.mjs`), so this gate asserts the DOM the
// page owns - the box geometry and the caption - rather than sampling the picture.
//
// The report itself is the engine's, so the numbers below also prove the round trip: the toolbar's
// `book` posture reaches `hal::fold_info_for`, the session lays the tree out at it, and
// `_tessera_canvas_overlay` reports the band the layout used.

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { createGate, launchGpuBrowser, pageLostDevice } from './browser_gpu.mjs';
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


// One full-bleed box: a posture change re-lays it out, so the session has a tree to report metrics for.
const PROJECT = {
  'tessera.json': `{
  "schemaVersion": 1,
  "app": { "name": "fold-overlay-check", "entry": "ui/App.tsx", "namespace": "fold_overlay_check" }
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

const gate = createGate();
const { check, section } = gate;

async function settles(expression, page, timeout = 20000, argument = undefined) {
  try {
    await page.waitForFunction(expression, argument, { timeout });
    return true;
  } catch {
    return false;
  }
}

/** Opens a Device submenu and clicks the item whose label matches `label` (the menu is label-driven). */
async function chooseFromDeviceMenu(page, submenu, label) {
  await page.click('#menubar-device');
  await settles(() => document.getElementById('menu-device') !== null, page, 5000);
  const row = page.locator('#menu-device [aria-haspopup="menu"]', { hasText: submenu });
  await row.hover();
  const item = page.locator('#menu-device [role="menuitemradio"], #menu-device [role="menuitem"]', {
    hasText: label,
  });
  await item.first().click();
}

/** The overlay's DOM as the page has it: whether it is shown, its boxes, and the caption. */
function readOverlay(page) {
  return page.evaluate(() => {
    const overlay = document.getElementById('fold-overlay');
    const boxes = [...overlay.querySelectorAll('.fold-overlay-box')].map((element) => ({
      className: element.className,
      left: element.style.left,
      top: element.style.top,
      width: element.style.width,
      height: element.style.height,
    }));
    return {
      hidden: overlay.hidden,
      boxes,
      caption: overlay.querySelector('.fold-overlay-caption')?.textContent ?? '',
      pressed: document.getElementById('fold-toggle')?.getAttribute('aria-pressed') ?? '',
      toggleLabel: document.getElementById('fold-toggle')?.textContent ?? '',
    };
  });
}

/** Waits until the frame counter has moved past `before`, i.e. a frame was actually painted. */
async function nextFrame(page, before) {
  return settles(
    (previous) =>
      (document.getElementById('stat-frame')?.textContent ?? '').trim() !== '' &&
      (document.getElementById('stat-frame')?.textContent ?? '').trim() !== previous,
    page,
    15000,
    before,
  );
}

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
    name: 'fold-overlay-check.zip',
    mimeType: 'application/zip',
    buffer: projectZip,
  });
  await settles(() => document.querySelector('#project-tree [data-path="ui/App.tsx"]') !== null, page);
  // Wait for an apply: the export answers the metrics of a laid-out tree, and the overlay's extent
  // check hides it until the session's metrics match the viewport.
  await settles(() => {
    const status = document.getElementById('canvas-status')?.textContent ?? '';
    return /draw call|drew|apply|not applied|fail|unavailable/i.test(status);
  }, page);
  return page;
}

const { server, base } = await startServer();
const browser = await launchGpuBrowser();

try {
  // The overlay needs a live device (see `section`, #958).
  await section('the fold overlay checks (#433)', async () => {
    const context = await browser.newContext({ viewport: { width: 1400, height: 1000 } });
    const page = await openCanvas(context, base);
    // Every check below reads the overlay the engine's metrics feed; a lost device means no metrics, so
    // the section is retried and, if the runner never holds a device, skipped rather than failed (#958).
    if (await pageLostDevice(page)) {
      await context.close();
      return true;
    }

    // --- off by default ----------------------------------------------------------------------------
    const before = await readOverlay(page);
    check('the overlay is off by default (#433)', before.hidden === true, `hidden=${before.hidden}`);
    check(
      'the toggle reads as unpressed (#433)',
      before.pressed === 'false',
      `aria-pressed=${before.pressed}`,
    );

    // --- a book fold draws its band ----------------------------------------------------------------
    // 1280x800 is the toolbar's desktop preset; the numbers below are its band geometry.
    await chooseFromDeviceMenu(page, 'Viewport', '1280x800');
    await settles(() => true, page, 300);
    const beforeFold = await frameCount(page);
    await chooseFromDeviceMenu(page, 'Fold Posture', 'Book');
    await nextFrame(page, beforeFold);

    await page.locator('#fold-toggle').click();
    // The toggle asks the engine and draws; a query, so no frame is owed and none is waited for.
    const armed = await settles(() => {
      const overlay = document.getElementById('fold-overlay');
      return overlay !== null && !overlay.hidden && overlay.querySelector('.fold-overlay-hinge') !== null;
    }, page, 5000);
    check('arming the overlay draws the hinge band (#433)', armed);
    const book = await readOverlay(page);
    check(
      'the toggle reads as pressed (#433)',
      book.pressed === 'true',
      `aria-pressed=${book.pressed}, label "${book.toggleLabel}"`,
    );
    const hinge = book.boxes.find((box) => box.className.includes('fold-overlay-hinge'));
    check(
      'the band is centred on the width, a 24 px hinge (#433)',
      // 1280 wide, a 24 px band centred: x = (1280 - 24) / 2 = 628, full height 800.
      hinge !== undefined &&
        Number.parseFloat(hinge.left) === 628 &&
        Number.parseFloat(hinge.width) === 24 &&
        Number.parseFloat(hinge.height) === 800,
      `band ${hinge ? `${hinge.left} x ${hinge.width} x ${hinge.height}` : 'missing'}`,
    );
    check(
      'the caption names the posture (#433)',
      /vertical hinge 24px/.test(book.caption),
      `caption "${book.caption}"`,
    );

    // --- tabletop is the other axis ----------------------------------------------------------------
    const beforeTabletop = await frameCount(page);
    await chooseFromDeviceMenu(page, 'Fold Posture', 'Tabletop');
    await nextFrame(page, beforeTabletop);
    const tabletop = await settles(() => {
      const caption = document.querySelector('#fold-overlay .fold-overlay-caption')?.textContent ?? '';
      return /horizontal hinge/.test(caption);
    }, page, 5000);
    check('a posture change redraws the overlay (#433)', tabletop);
    const tabletopBox = (await readOverlay(page)).boxes.find((box) =>
      box.className.includes('fold-overlay-hinge'),
    );
    check(
      'a tabletop band spans the width and is centred on the height (#433)',
      // 800 tall, a 24 px band centred: y = (800 - 24) / 2 = 388, full width 1280.
      tabletopBox !== undefined &&
        Number.parseFloat(tabletopBox.top) === 388 &&
        Number.parseFloat(tabletopBox.width) === 1280 &&
        Number.parseFloat(tabletopBox.height) === 24,
      `band ${tabletopBox ? `${tabletopBox.top} x ${tabletopBox.width} x ${tabletopBox.height}` : 'missing'}`,
    );

    // --- the band is page DOM, not engine paint ----------------------------------------------------
    //
    // The frame counter must not move because the overlay was armed: the overlay is a query over the
    // snapshot, and an export that dispatched (or marked a dirty domain) would cost a frame here.
    await page.locator('#fold-toggle').click();
    await settles(() => document.getElementById('fold-overlay').hidden === true, page, 3000);
    const offFrame = await frameCount(page);
    await page.locator('#fold-toggle').click();
    await settles(() => document.getElementById('fold-overlay').hidden === false, page, 3000);
    const onFrame = await frameCount(page);
    check(
      'arming the overlay costs no frame (#433)',
      onFrame === offFrame,
      `frame ${offFrame} -> ${onFrame}`,
    );

    // --- flat draws nothing, and names the posture -------------------------------------------------
    const beforeFlat = await frameCount(page);
    await chooseFromDeviceMenu(page, 'Fold Posture', 'Flat');
    await nextFrame(page, beforeFlat);
    const flat = await settles(() => {
      const caption = document.querySelector('#fold-overlay .fold-overlay-caption')?.textContent ?? '';
      return caption === 'flat';
    }, page, 5000);
    check('a flat fold still names its posture (#433)', flat);
    const flatBoxes = (await readOverlay(page)).boxes.filter((box) =>
      box.className.includes('fold-overlay-hinge'),
    );
    check('a flat fold draws no band (#433)', flatBoxes.length === 0, `${flatBoxes.length} band(s)`);

    // --- disarming hides it again ------------------------------------------------------------------
    await page.locator('#fold-toggle').click();
    await settles(() => document.getElementById('fold-overlay').hidden === true, page, 3000);
    const disarmed = await readOverlay(page);
    check('disarming hides the overlay (#433)', disarmed.hidden === true);
    check(
      'the toggle reads as unpressed again (#433)',
      disarmed.pressed === 'false',
      `aria-pressed=${disarmed.pressed}`,
    );

    if (shot) {
      await page.screenshot({ path: shot, fullPage: false });
      process.stdout.write(`shot: ${shot}\n`);
    }

    const lost = await pageLostDevice(page);
    await context.close();
    return lost;
  });
} finally {
  await browser.close();
  server.close();
}

process.exit(gate.finish('fold overlay'));
