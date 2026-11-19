#!/usr/bin/env node
/**
 * Issue #436 in a real browser: **a device preset re-resolves `@media` and re-lays the running
 * preview out in place**, so the painted colour follows the breakpoint, the state cell survives, and
 * each switch costs exactly one frame and no re-evaluation.
 *
 * **Why this needs its own probe.** Every other browser script proves the page *works*: the module
 * loads, a frame presents, an edit applies, a share link round-trips. None of them can see the thing
 * this issue is about, because before #436 a viewport change *looked* fine: the boxes re-laid out
 * and the stylesheet silently kept its startup answer, so a phone preset showed a desktop's colours
 * at a phone's size. A probe that only checked "the canvas repainted" would have passed.
 *
 * What it checks, in one run, with no Restart pressed between switches:
 *
 *   1. the phone preset (390x844) paints the `@media (max-width: 600px)` colour;
 *   2. the desktop preset (1280x800) paints the base colour;
 *   3. switching between two sizes on the SAME side of the breakpoint (desktop -> tablet) changes no
 *      colour -- that is the "crossing" rule, observed rather than assumed;
 *   4. the state cell keeps its value across every switch (a re-evaluation would re-run the
 *      component; the engine's own counter and the state row say it did not);
 *   5. the frame counter moves by exactly ONE per switch, and the cascade generation moves by
 *      exactly one per CROSSED breakpoint and not at all for a same-side move.
 *
 * **Why the colour comes from a screenshot.** A WebGPU canvas whose frame has been presented cannot
 * be read back with `drawImage`/`getImageData`: the swapchain texture is already consumed, so every
 * read returns transparent black. Playwright's element screenshot captures the composited page, so
 * the pixels it contains are the pixels a person sees. The PNG is decoded by the browser itself
 * (`createImageBitmap`), so no image library is needed here.
 *
 * Developer-run and not a CI gate, for the same reason the other browser scripts are: it needs a
 * browser and a WebGPU-capable driver, which no lane has today.
 *
 * Usage:
 *   node scripts/ci/preview_browser_viewport.mjs build/pages [--shots build/]
 */

import { createServer } from 'node:http';
import { readFile, mkdir } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { editEntry, editSheet } from './studio_project.mjs';

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
};

function argument(name, fallback) {
  const index = process.argv.indexOf(name);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
}

const positional = process.argv.slice(2).filter((value) => !value.startsWith('--'));
const site = argument('--site', positional[0] ?? 'build/pages');
const shots = argument('--shots', null);

let chromium;
try {
  ({ chromium } = await import('playwright'));
} catch {
  console.error(
    'playwright is not installed. This script is developer-run on purpose (no repository dependency):\n' +
      '  npm install playwright && npx playwright install chromium\n' +
      'Run it from a directory whose node_modules has playwright.',
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
if (shots) {
  await mkdir(shots, { recursive: true });
}

const browser = await chromium.launch({
  args: [
    '--enable-unsafe-webgpu',
    '--enable-features=Vulkan',
    '--use-angle=swiftshader',
    '--disable-vulkan-surface',
  ],
});
const page = await (await browser.newContext({ viewport: { width: 1440, height: 900 } })).newPage();

let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok   ' : 'FAIL '} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
};

const consoleErrors = [];
page.on('console', (message) => {
  if (message.type() === 'error') consoleErrors.push(message.text());
});
page.on('pageerror', (error) => consoleErrors.push(`pageerror: ${String(error)}`));

const text = async (selector) =>
  page.evaluate((id) => document.querySelector(id)?.textContent?.trim() ?? null, selector);

/** `#stat-frame` reads "<w>x<h> (#N, cM)": the session's frame count and cascade generation. */
async function counters() {
  const raw = (await text('#stat-frame')) ?? '';
  const size = /(\d+)x(\d+)/.exec(raw);
  const frames = /#(\d+)/.exec(raw);
  const cascade = /c(\d+)/.exec(raw);
  return {
    raw,
    width: size ? Number(size[1]) : null,
    height: size ? Number(size[2]) : null,
    frames: frames ? Number(frames[1]) : null,
    cascade: cascade ? Number(cascade[1]) : null,
  };
}

/** The single scalar state cell's value, straight off the row the pane mirrors it to. */
const stateValue = async () =>
  page.evaluate(() => document.querySelector('#stat-state')?.getAttribute('data-value') ?? null);

/** The average colour of the canvas element as the page composites it, decoded from a screenshot. */
async function canvasColour(label) {
  const options = shots ? { path: join(shots, `436-${label}.png`) } : {};
  const png = await page.locator('#canvas').screenshot(options);
  const base64 = png.toString('base64');
  return page.evaluate(async (data) => {
    const bytes = Uint8Array.from(atob(data), (c) => c.charCodeAt(0));
    const bitmap = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
    const scratch = document.createElement('canvas');
    scratch.width = 16;
    scratch.height = 16;
    const ctx = scratch.getContext('2d');
    ctx.drawImage(bitmap, 0, 0, 16, 16);
    const pixels = ctx.getImageData(0, 0, 16, 16).data;
    let r = 0;
    let g = 0;
    let b = 0;
    for (let i = 0; i < pixels.length; i += 4) {
      r += pixels[i];
      g += pixels[i + 1];
      b += pixels[i + 2];
    }
    const n = pixels.length / 4;
    return { r: Math.round(r / n), g: Math.round(g / n), b: Math.round(b / n) };
  }, base64);
}

const describe = (colour) => `rgb(${colour.r}, ${colour.g}, ${colour.b})`;
const bluish = (colour) => colour.b > colour.r + 10;
const reddish = (colour) => colour.r > colour.b + 60;

/** Waits until the pane's frame counter moves past `before`, or gives up after the timeout. */
async function settle(before, timeoutMs = 4000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const now = await counters();
    if (before === null || (now.frames ?? 0) > before) {
      await page.waitForTimeout(250);
      return counters();
    }
    await page.waitForTimeout(100);
  }
  return counters();
}

// The page under test: a `page` class whose background a width breakpoint changes, and a state cell
// so "the preview did not re-run" has something to keep.
const SOURCE = `export function Screen(): Node {
  const [count, setCount] = useState<int>(7);
  return (
    <View className="page">
      <Text className="title">Viewport</Text>
      <Button>Go</Button>
    </View>
  );
}
`;

const STYLESHEET = `.page { display: flex; flex-direction: column; width: 100%; height: 100%; background-color: rgb(20, 40, 60); }
.title { height: 24px; color: rgb(255, 255, 255); }
Button { width: 120px; height: 32px; background-color: rgb(45, 108, 223); }
@media (max-width: 600px) {
  .page { background-color: rgb(200, 30, 30); }
}
`;

await page.goto(base, { waitUntil: 'load' });
await page.waitForTimeout(800);
await page.locator('#tab-canvas').click();
await page.waitForTimeout(600);

if (!(await page.locator('#unsupported').isHidden())) {
  console.log('this browser has no WebGPU, so the canvas cannot be checked');
  await browser.close();
  server.close();
  process.exit(2);
}

// --- the fixture, at the desktop preset ---------------------------------------------------------
await page.evaluate(() => window.__tesseraWorkbench.run('device.setViewport', { value: '1280x800' }));
await editEntry(page, SOURCE);
await editSheet(page, STYLESHEET);
// One explicit cold start, and never again: every switch below is a toolbar change.
await page.locator('#restart-app').click();
await page.waitForTimeout(3000);

const wideCounters = await counters();
const wideColour = await canvasColour('desktop-1280x800');
console.log(`desktop 1280x800: ${describe(wideColour)} ${wideCounters.raw}`);
check(
  'the desktop preset paints the base colour (blue-dominant, not the breakpoint red)',
  bluish(wideColour) && !reddish(wideColour),
  describe(wideColour),
);
check(
  'the canvas is laid out at the preset size, not at a default 320x240',
  wideCounters.width === 1280 && wideCounters.height === 800,
  wideCounters.raw,
);
check('the fixture has a state cell to keep', (await stateValue()) === '7', `state=${await stateValue()}`);
const baseline = wideCounters;

// --- a same-side switch: no colour change, one frame, no cascade rebuild -------------------------
await page.evaluate(() => window.__tesseraWorkbench.run('device.setViewport', { value: '768x1024' }));
const tabletCounters = await settle(baseline.frames);
const tabletColour = await canvasColour('tablet-768x1024');
console.log(`tablet 768x1024: ${describe(tabletColour)} ${tabletCounters.raw}`);
check(
  'a size on the same side of the breakpoint keeps the colour (no crossing)',
  bluish(tabletColour) && !reddish(tabletColour),
  describe(tabletColour),
);
check(
  'a same-side switch still re-lays out: the canvas follows 768x1024',
  tabletCounters.width === 768 && tabletCounters.height === 1024,
  tabletCounters.raw,
);
check(
  'a same-side switch costs exactly one frame',
  tabletCounters.frames === baseline.frames + 1,
  `${baseline.frames} -> ${tabletCounters.frames}`,
);
check(
  'a same-side switch does NOT rebuild the cascade',
  tabletCounters.cascade === baseline.cascade,
  `cascade ${baseline.cascade} -> ${tabletCounters.cascade}`,
);
check('the state cell survives the same-side switch', (await stateValue()) === '7');

// --- crossing the breakpoint: the colour must move ----------------------------------------------
await page.evaluate(() => window.__tesseraWorkbench.run('device.setViewport', { value: '390x844' }));
const phoneCounters = await settle(tabletCounters.frames);
const phoneColour = await canvasColour('phone-390x844');
console.log(`phone 390x844: ${describe(phoneColour)} ${phoneCounters.raw}`);
check(
  'the phone preset paints the @media (max-width: 600px) colour (red-dominant)',
  reddish(phoneColour),
  describe(phoneColour),
);
check(
  'the painted colour actually changed across the breakpoint',
  Math.abs(phoneColour.r - tabletColour.r) > 60,
  `${describe(tabletColour)} -> ${describe(phoneColour)}`,
);
check(
  'a crossing switch costs exactly one frame (no re-evaluation burst)',
  phoneCounters.frames === tabletCounters.frames + 1,
  `${tabletCounters.frames} -> ${phoneCounters.frames}`,
);
check(
  'crossing the breakpoint rebuilds the cascade exactly once',
  phoneCounters.cascade === tabletCounters.cascade + 1,
  `cascade ${tabletCounters.cascade} -> ${phoneCounters.cascade}`,
);
check('the state cell survives the breakpoint (in-place resize, no re-run)', (await stateValue()) === '7');

// --- back to the desktop: one more crossing, one more frame, the base colour ---------------------
await page.evaluate(() => window.__tesseraWorkbench.run('device.setViewport', { value: '1280x800' }));
const backCounters = await settle(phoneCounters.frames);
const backColour = await canvasColour('desktop-again');
console.log(`desktop again: ${describe(backColour)} ${backCounters.raw}`);
check(
  'switching back restores the base colour',
  bluish(backColour) && !reddish(backColour),
  describe(backColour),
);
check(
  'switching back costs exactly one frame',
  backCounters.frames === phoneCounters.frames + 1,
  `${phoneCounters.frames} -> ${backCounters.frames}`,
);
check(
  'switching back rebuilds the cascade exactly once',
  backCounters.cascade === phoneCounters.cascade + 1,
  `cascade ${phoneCounters.cascade} -> ${backCounters.cascade}`,
);
check('the state cell is still 7 after three switches', (await stateValue()) === '7');

check('no console errors', consoleErrors.length === 0, consoleErrors.slice(0, 3).join(' | ') || 'none');

console.log(`\n${failures === 0 ? 'all viewport checks passed' : `${failures} viewport check(s) failed`}`);
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
