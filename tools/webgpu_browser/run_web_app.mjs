// SPDX-License-Identifier: MIT
//
// Drives a built web app (the wasm-webgpu preset's samples/web_render folder, issue #1013) in a real
// Chromium through Playwright and checks what only a browser can: the WebGPU device and canvas
// surface submit frames, a pointer change repaints once, and an idle page holds no requestAnimationFrame.
// Dev tooling, not shipped; CI has no WebGPU adapter, so this is run by hand (docs/design/web-target.md).
//
//   node run_web_app.mjs <app dir> [--out dir] [--flags a,b] [--pixels]
//
// Needs the `playwright` package (NODE_PATH or a local node_modules) and its Chromium. Software
// WebGPU (SwiftShader) is enough for the frame accounting; the pixel check (--pixels) needs a GPU.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const argv = process.argv.slice(2);
const dir = path.resolve(argv[0] ?? '');
const flag = (name, fallback) => (argv.includes(name) ? argv[argv.indexOf(name) + 1] : fallback);
const out = path.resolve(flag('--out', '.'));
const extra = flag('--flags', '').split(',').filter(Boolean);
if (!argv[0] || !fs.existsSync(path.join(dir, 'app.js'))) {
  console.error('usage: run_web_app.mjs <app dir with app.js> [--out dir] [--flags a,b]');
  process.exit(2);
}

const types = { '.js': 'text/javascript', '.wasm': 'application/wasm', '.html': 'text/html' };
const server = http.createServer((req, res) => {
  const name = path.join(dir, req.url === '/' ? 'index.html' : path.basename(req.url.split('?')[0]));
  if (!fs.existsSync(name)) { res.statusCode = 404; return res.end('missing'); }
  res.setHeader('Content-Type', types[path.extname(name)] ?? 'application/octet-stream');
  res.end(fs.readFileSync(name));
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${server.address().port}/`;

let failed = false;
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); failed ||= !ok; };

// `channel: 'chromium'` is the full Chromium in new headless mode; the default headless shell does not
// composite WebGPU canvases into screenshots.
const browser = await chromium.launch({
  channel: 'chromium',
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader',
    '--use-webgpu-adapter=swiftshader', '--ignore-gpu-blocklist', ...extra],
});
try {
  const page = await browser.newPage({ viewport: { width: 400, height: 300 }, deviceScaleFactor: 1 });
  const logs = [];
  page.on('console', (m) => logs.push(m.text()));
  page.on('pageerror', (e) => logs.push('pageerror: ' + e.message + ' ' + (e.stack ?? '')));
  await page.addInitScript(() => {
    window.__raf = 0;
    const raf = window.requestAnimationFrame.bind(window);
    window.requestAnimationFrame = (cb) => { window.__raf += 1; return raf(cb); };
  });
  await page.goto(url);
  await page.waitForTimeout(2500);
  const supported = await page.evaluate(() => Boolean(navigator.gpu));
  check(supported, 'navigator.gpu present');
  const unsupportedShown = await page.evaluate(() => !document.getElementById('unsupported').hidden);
  check(!unsupportedShown, 'the unsupported page is not shown');
  fs.mkdirSync(out, { recursive: true });

  const frames = () => logs.filter((l) => l.startsWith('web_render: canvas frame')).length;
  const settle = () => page.waitForTimeout(400);

  check(frames() >= 1, `the first canvas frame was submitted (${frames()} so far)`);
  check(logs.some((l) => /canvas frame 1 at 400x300 scale 1\.00/.test(l)), 'the backing store is 400x300 at scale 1');
  check(!logs.some((l) => l.startsWith('pageerror') || l.includes('frame failed') || l.includes('no renderer')),
    'no page error, failed frame or missing renderer');

  // Each pointer change that alters the button (hover in, press, release, leave) is exactly one
  // canvas frame; a move that changes nothing is none.
  let count = frames();
  const step = async (what, expected, act) => {
    await act();
    await settle();
    const now = frames();
    check(now - count === expected, `${what}: ${now - count} canvas frame(s), expected ${expected}`);
    count = now;
  };
  await step('hover into the button', 1, () => page.mouse.move(100, 48));
  await step('move inside the button', 0, () => page.mouse.move(110, 50));
  await step('press', 1, () => page.mouse.down());
  await step('release', 1, () => page.mouse.up());
  await step('leave the button', 1, () => page.mouse.move(350, 280));
  await step('move outside the button', 0, () => page.mouse.move(360, 290));

  // Pixels only mean something on a real GPU compositor: headless software WebGPU screenshots show
  // the canvas as blank, so this is opt-in (--pixels) and not part of the default run.
  if (argv.includes('--pixels')) {
    const pixel = async (name, x, y) => {
      const shot = await page.screenshot({ path: path.join(out, name + '.png') });
      return page.evaluate(async ([bytes, px, py]) => {
        const bitmap = await createImageBitmap(new Blob([new Uint8Array(bytes)], { type: 'image/png' }));
        const c = new OffscreenCanvas(bitmap.width, bitmap.height).getContext('2d');
        c.drawImage(bitmap, 0, 0);
        return Array.from(c.getImageData(px, py, 1, 1).data);
      }, [Array.from(shot), x, y]);
    };
    const near = (a, b, tol = 6) => a.slice(0, 3).every((v, i) => Math.abs(v - b[i]) <= tol);
    const background = await pixel('background', 300, 250);
    check(near(background, [244, 245, 247]), 'canvas background drawn ' + background.slice(0, 3));
    const card = await pixel('card', 280, 90);
    check(near(card, [255, 255, 255]), 'card drawn ' + card.slice(0, 3));
    const button = await pixel('button', 100, 48);
    check(near(button, [37, 99, 235]), 'button drawn ' + button.slice(0, 3));
  }

  const before = await page.evaluate(() => window.__raf);
  await page.waitForTimeout(1500);
  const after = await page.evaluate(() => window.__raf);
  check(after === before, `an idle page requests no animation frame (${before} -> ${after})`);
  if (logs.length) console.log('page log:\n' + logs.join('\n'));
} finally {
  await browser.close();
  server.close();
}
process.exit(failed ? 1 : 0);
