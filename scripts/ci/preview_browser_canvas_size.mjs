// SPDX-License-Identifier: MIT
//
// CI browser acceptance for the canvas' size: #598's backing store, and the drag-resize handles
// (issue #690 slice b).
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
const browser = await launchGpuBrowser();

try {
  // --- #598: the backing store follows the viewport ------------------------------------------------
  //
  // This half needs a live device: a lost device clears the session's `gpu` and a viewport change is then
  // not applied (see `section`, #958), so a run that lost it is retried or skipped loudly.
  await section('the viewport sizing checks (#598)', async () => {
  const sizingContext = await browser.newContext({ viewport: { width: 1400, height: 1000 } });
  const sizingPage = await openCanvas(sizingContext, base);
  // No device from the start: nothing below can be judged, and every toolbar move would be a no-op.
  if (await pageLostDevice(sizingPage)) {
    await sizingContext.close();
    return true;
  }

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
  const lostSizing = await pageLostDevice(sizingPage);
  await sizingContext.close();
  return lostSizing;
  });

  // --- DPR 2 geometry: the store and the box both double ------------------------------------------
  //
  // The PAINT at DPR 2 is `scene_frame_parity.py`'s `dpr` case (lavapipe): a presented WebGPU frame
  // cannot be read back in headless Chromium, so the assertion here stops at the two numbers the
  // page owns - the backing store and the CSS box - which is where #598 and #599 both started.
  await section('the DPR and drag-resize checks (#598, #599, #690)', async () => {
  const scaledContext = await browser.newContext({ viewport: { width: 1400, height: 1000 } });
  const scaledPage = await openCanvas(scaledContext, base);
  if (await pageLostDevice(scaledPage)) {
    await scaledContext.close();
    return true;
  }

  const before768 = await frameCount(scaledPage);
  await chooseFromDeviceMenu(scaledPage, 'Viewport', '768x1024');
  await nextFrame(scaledPage, before768);
  const before2 = await frameCount(scaledPage);
  await chooseFromDeviceMenu(scaledPage, 'Pixel Ratio', '2');
  await nextFrame(scaledPage, before2);
  // Lost between the preset and the DPR change: the changes were not applied, which is the runner's doing.
  if (await pageLostDevice(scaledPage)) {
    await scaledContext.close();
    return true;
  }

  // The pane writes `#stat-dpr` when the frame that applies the new ratio is drawn, while the backing
  // store is resized synchronously by the toolbar handler. `nextFrame` can return on a frame that was
  // already in flight for the preset, so the ratio the pane reports is awaited itself: reading it
  // early gave "DPR 1" next to a 1536px store on a slower runner.
  await settles(() => (document.getElementById('stat-dpr')?.textContent ?? '').trim() === '2', scaledPage, 15000);
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
  // --- the drag-resize handles (issue #690 slice b) ------------------------------------------------
  //
  // The arithmetic is gated in Node (`preview_canvas_resize.mjs`); what only a browser can show is
  // that the grips are wired at all - the session is created before React commits them, so a listener
  // looked up by id finds nothing - and that a burst of pointer events costs one frame.
  await chooseFromDeviceMenu(scaledPage, "Viewport", "320x240");
  // Wait for the pane's zoom to settle on the 320x240 stage. This used to be `settles(() => true, ...)`, which
  // resolves at once and waited for nothing: the drag below then started while the stage was still scaled
  // for the 768x1024 viewport before it, and the drag (correctly) divides pointer travel by the live zoom,
  // so the released size came out 544x408 instead of 520x390. At zoom 1 the stage's rect equals its layout box.
  const stageSettled = await settles(() => {
    const stage = document.querySelector('.canvas-zoom-stage');
    const canvas = document.getElementById('canvas');
    return (
      stage !== null &&
      canvas.style.width === '320px' &&
      stage.offsetWidth === 320 &&
      Math.abs(stage.getBoundingClientRect().width - 320) < 0.5
    );
  }, scaledPage, 5000);
  check('the 320x240 stage is unscaled before the drag (#690)', stageSettled);

  const beforeDrag = await frameCount(scaledPage);
  const grip = await scaledPage.$("#canvas-resize-corner");
  check("the corner grip is rendered (#690)", grip !== null);

  if (grip !== null) {
    // 50 moves dispatched in ONE task: the case Playwright's mouse API cannot produce (each of its
    // moves round-trips and lands in its own frame) and the case the coalescing exists for.
    const drag = await scaledPage.evaluate(async () => {
      const element = document.getElementById("canvas-resize-corner");
      const box = element.getBoundingClientRect();
      const x = box.x + 6;
      const y = box.y + 6;
      const send = (type, cx, cy) =>
        element.dispatchEvent(
          new PointerEvent(type, {
            bubbles: true,
            cancelable: true,
            clientX: cx,
            clientY: cy,
            pointerId: 7,
            isPrimary: true,
            button: 0,
            buttons: 1,
          }),
        );
      const readFrame = () =>
        document.getElementById("stat-frame").textContent ?? "";
      send("pointerdown", x, y);
      const during = document.getElementById("canvas-resize-readout").textContent ?? "";
      for (let i = 1; i <= 50; i += 1) {
        send("pointermove", x + i * 4, y + i * 3);
      }
      const readout = document.getElementById("canvas-resize-readout").textContent ?? "";
      // The chain is React commit -> effect -> `requestFrame` -> rAF -> draw, so a few frames are
      // needed before a mid-drag paint is observable at all. A single frame reads the state before
      // the drag, which is how an earlier version of this check passed while proving nothing.
      for (let frame = 0; frame < 6; frame += 1) {
        await new Promise((done) => requestAnimationFrame(done));
      }
      const mid = readFrame();
      send("pointerup", x + 200, y + 150);
      await new Promise((done) => requestAnimationFrame(() => requestAnimationFrame(done)));
      return { during, readout, mid, after: readFrame() };
    });
    const settled = await readCanvas(scaledPage);
    const afterDrag = await frameCount(scaledPage);
    check(
      "the readout tracks the pending size while dragging (#690)",
      drag.readout !== "" && drag.readout !== drag.during,
      `readout went from "${drag.during}" to "${drag.readout}"`,
    );
    // **What this number can and cannot show.** It bounds the frames a drag costs, which is the user
    // -visible property. It does NOT prove the per-frame step in `canvas-resize-layer.ts` is doing
    // anything: the session's own `requestFrame()` already coalesces frames, so a per-move `apply`
    // measures exactly the same here (mutation-checked: replacing `schedule()` with `step(false)`
    // leaves this check green). The timing rule itself is gated in Node, where it can be observed
    // (`preview_canvas_resize.mjs`: five moves in one frame cost one step).
    check(
      "a drag of 50 moves in one task costs at most a frame per paint (#690)",
      afterDrag - beforeDrag <= 3,
      `frame ${beforeDrag} -> ${afterDrag} for 50 moves`,
    );
    check(
      "the released size is exact, not the last animation frame's (#690)",
      settled.cssWidth === "520px" && settled.cssHeight === "390px",
      `320x240 + 200x150 = 520x390, got ${settled.cssWidth} x ${settled.cssHeight}`,
    );
  }

  if (shot) {
    await scaledPage.screenshot({ path: shot, fullPage: false });
    process.stdout.write(`shot: ${shot}\n`);
  }
  const lostScaled = await pageLostDevice(scaledPage);
  await scaledContext.close();
  return lostScaled;
  });
} finally {
  await browser.close();
  server.close();
}

process.exit(gate.finish('canvas size'));
