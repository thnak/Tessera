#!/usr/bin/env node
/**
 * Read the centre pixel of a transparent box that has an outer `box-shadow`, in the assembled studio
 * page (issue #461).
 *
 * **Why this exists.** CSS paints an outer shadow only outside the border box (CSS Backgrounds 3
 * section 7.1.1), so a transparent box with a shadow shows the page through it in a browser. The
 * engine used to paint the shadow inside its own box, which a transparent box showed as a grey
 * patch. Unit tests and the CPU golden cover the geometry; this probe proves the *page*: the studio
 * compiles the source below in the browser, the canvas draws it on WebGPU, and the pixels are read
 * back from an **element screenshot** (a presented WebGPU swapchain returns transparent black to
 * `drawImage`, so the canvas is captured, not read).
 *
 * Developer-run, like `preview_browser_verify.mjs`: it needs a browser with a WebGPU driver.
 *
 * Usage:
 *   node scripts/ci/preview_browser_shadow.mjs --site build/pages [--keep] [--shot out.png]
 *
 * The site must be the assembled artifact (the built front end plus the three Wasm modules), served
 * over HTTP because a Wasm module cannot be fetched from `file://`.
 */

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { editEntry, editSheet, ensureProject, tab } from './studio_project.mjs';

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

// The box the probe paints: 160x100 at (80, 70) in the 320x240 preview viewport, with a bright
// shadow on the dark page and NO background, so its interior must be exactly the page colour and
// the shadow band below it must be visibly blue.
const kBox = { x: 80, y: 70, width: 160, height: 100 };
const CSS = `.shadow-box {
  width: ${kBox.width}px;
  height: ${kBox.height}px;
  margin-left: ${kBox.x}px;
  margin-top: ${kBox.y}px;
  background-color: transparent;
  border-radius: 12px;
  box-shadow: 0 4px 20px rgba(56, 189, 248, 0.55);
}
`;
const SOURCE = `export function ShadowBox(): Node {
  return (
    <View className="shadow-box">
      <Text>shadow</Text>
    </View>
  );
}
`;

// Probe points in canvas coordinates. The element screenshot may include a one-pixel element
// border, so the points are read relative to the image size instead of assuming a pixel offset.
const PROBES = {
  centre: [kBox.x + kBox.width / 2, kBox.y + kBox.height / 2],
  insideTopLeft: [kBox.x + 8, kBox.y + 8],
  insideBottomEdge: [kBox.x + kBox.width / 2, kBox.y + kBox.height - 4],
  shadowBandNear: [kBox.x + kBox.width / 2, kBox.y + kBox.height + 10],
  shadowBandFar: [kBox.x + kBox.width / 2, kBox.y + kBox.height + 16],
  page: [10, kBox.y - 20],
};

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
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader',
         '--disable-vulkan-surface'],
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
const rgb = (pixel) => `rgb(${pixel[0]}, ${pixel[1]}, ${pixel[2]})`;

await page.goto(`${base}/index.html`, { waitUntil: 'load' });
await page.waitForTimeout(1200);
// No project boots by default (issue #391); open one so the diagnostic pane has an evaluation (issue #849).
await ensureProject(page);

const hasGpu = await page.evaluate(async () => {
  if (!navigator.gpu) return false;
  const adapter = await navigator.gpu.requestAdapter();
  return adapter ? Boolean(await adapter.requestDevice()) : false;
});
check('WebGPU device is available', hasGpu);

// The first load applies itself once the modules are ready (#444); wait for that before editing.
await page.waitForFunction(
  () => {
    const text = document.querySelector('#diag-status')?.textContent ?? '';
    return text !== '' && text !== 'not run yet';
  },
  undefined,
  { timeout: 15000 },
);

await tab(page, 'canvas');
await page.waitForTimeout(1200);

// The author stylesheet goes first: the source's `onInput` is what triggers the debounced apply.
await editSheet(page,CSS);
await editEntry(page, SOURCE);
await page.waitForFunction(
  () => /draw call/.test(document.querySelector('#canvas-status')?.textContent ?? ''),
  undefined,
  { timeout: 20000 },
);
await page.waitForTimeout(1500);

const canvasStatus = await page.evaluate(
  () => document.querySelector('#canvas-status')?.textContent ?? '',
);
check('The preview drew the probe source', /draw call/.test(canvasStatus), canvasStatus);

// An element screenshot, decoded through a 2D canvas in the page: reading the WebGPU swapchain
// directly would hand back transparent black.
const png = await page.locator('#canvas').screenshot();
const dataUrl = `data:image/png;base64,${png.toString('base64')}`;
if (shot) {
  const { writeFile } = await import('node:fs/promises');
  await writeFile(shot, png);
  console.log(`screenshot: ${shot}`);
}
const sample = await page.evaluate(
  async ({ url, points }) => {
    const image = new Image();
    await new Promise((resolve, reject) => {
      image.onload = resolve;
      image.onerror = () => reject(new Error('cannot decode the canvas screenshot'));
      image.src = url;
    });
    const surface = document.createElement('canvas');
    surface.width = image.width;
    surface.height = image.height;
    const context = surface.getContext('2d', { willReadFrequently: true });
    context.drawImage(image, 0, 0);
    const read = (x, y) => Array.from(
      context.getImageData(Math.round(x), Math.round(y), 1, 1).data,
    );
    const result = { width: image.width, height: image.height, pixels: {} };
    for (const [name, point] of Object.entries(points)) {
      result.pixels[name] = read(point[0], point[1]);
    }
    return result;
  },
  { url: dataUrl, points: PROBES },
);

const pagePixel = sample.pixels.page;
console.log(`\ncanvas ${sample.width}x${sample.height}, page colour ${rgb(pagePixel)}`);
for (const [name, point] of Object.entries(PROBES)) {
  console.log(`  ${name.padEnd(16)} (${point[0]}, ${point[1]}) -> ${rgb(sample.pixels[name])}`);
}

const same = (a, b) =>
  Math.abs(a[0] - b[0]) <= 1 && Math.abs(a[1] - b[1]) <= 1 && Math.abs(a[2] - b[2]) <= 1;
const bluer = (a, b) => a[2] > b[2] + 2;

check('The canvas is the preview viewport',
  sample.width >= 320 && sample.width <= 322 && sample.height >= 240 && sample.height <= 242,
  `${sample.width}x${sample.height}`);
check('A transparent box paints the page colour at its centre',
  same(sample.pixels.centre, pagePixel),
  `${rgb(sample.pixels.centre)} vs page ${rgb(pagePixel)}`);
check('A transparent box paints the page colour inside its whole border box',
  same(sample.pixels.insideTopLeft, pagePixel) && same(sample.pixels.insideBottomEdge, pagePixel),
  `${rgb(sample.pixels.insideTopLeft)} / ${rgb(sample.pixels.insideBottomEdge)}`);
check('The shadow still paints below the box',
  bluer(sample.pixels.shadowBandNear, pagePixel) || bluer(sample.pixels.shadowBandFar, pagePixel),
  `${rgb(sample.pixels.shadowBandNear)} / ${rgb(sample.pixels.shadowBandFar)} vs page`);
check('No console errors', consoleErrors.length === 0,
  consoleErrors.slice(0, 3).join(' | ') || 'none');

console.log(`\n${failures === 0 ? 'all shadow checks passed' : `${failures} check(s) failed`}`);

if (!process.argv.includes('--keep')) {
  await browser.close();
  server.close();
}
process.exit(failures === 0 ? 0 : 1);
