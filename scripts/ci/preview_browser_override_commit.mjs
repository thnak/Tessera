// SPDX-License-Identifier: MIT
//
// Developer-run browser acceptance for the style override layer wired into the page (issue #585): edit a
// colour in the inspector, see the frame repaint, commit it, see the file text change, and see the app keep
// its state (no full re-init). Also the Undo/Redo routing (rule D3) in a real focus model, a stale-buffer
// commit being rejected, and screenshots of the inspector in light, dark and 390 px.
//
//   npm run build --prefix designer/web
//   (assemble the site: designer/web/dist + samples/ + tessera_canvas.{js,wasm} from the Wasm build)
//   PLAYWRIGHT_DIR=<dir whose node_modules holds playwright> [PLAYWRIGHT_CHROMIUM=<chrome>] \
//     node scripts/ci/preview_browser_override_commit.mjs --site <assembled site> [--shots dir]
//
// Needs the Wasm canvas module built from a tree that has the override exports (main since #553) and a
// Chromium with WebGPU (swiftshader is enough). Not run in CI until the Wasm lane runs browser gates.

import { createRequire } from 'node:module';
import { createServer } from 'node:http';
import { mkdir, readFile } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const args = process.argv.slice(2);
const arg = (name, fallback) => (args.includes(name) ? args[args.indexOf(name) + 1] : fallback);
const site = arg('--site', 'build/pages');
const shots = arg('--shots', null);
if (shots) await mkdir(shots, { recursive: true });

async function loadChromium() {
  let mod;
  try {
    mod = await import('playwright');
  } catch {
    const dir = process.env.PLAYWRIGHT_DIR;
    if (!dir) {
      console.error('playwright not found: npm install playwright, or set PLAYWRIGHT_DIR');
      process.exit(2);
    }
    const require = createRequire(join(resolve(dir), 'noop.js'));
    mod = await import(pathToFileURL(require.resolve('playwright')).href);
  }
  return mod.chromium ?? mod.default.chromium;
}

const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.wasm': 'application/wasm', '.ttf': 'font/ttf', '.txt': 'text/plain', '.tsx': 'text/plain' };
const server = createServer(async (request, response) => {
  let path = normalize(decodeURIComponent(new URL(request.url, 'http://x').pathname));
  if (path === '/' || path === '\\') path = '/index.html';
  try {
    const body = await readFile(join(site, path));
    response.writeHead(200, { 'content-type': MIME[extname(path)] ?? 'application/octet-stream', 'cache-control': 'no-store' });
    response.end(body);
  } catch {
    response.writeHead(404, { 'content-type': 'text/plain' });
    response.end('not found');
  }
});
await new Promise((done) => server.listen(0, '127.0.0.1', done));
const base = `http://127.0.0.1:${server.address().port}`;

const chromium = await loadChromium();
const browser = await chromium.launch({
  executablePath: process.env.PLAYWRIGHT_CHROMIUM || undefined,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader'],
});
const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
const page = await context.newPage();
const errors = [];
page.on('pageerror', (error) => errors.push(String(error)));
page.on('console', (message) => message.type() === 'error' && !/404/.test(message.text()) && errors.push(message.text()));

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
}

const editorText = () => page.evaluate(() => window.__tesseraEditor.text());
const editKeys = () =>
  page.evaluate(() => {
    const c = window.__tesseraWorkbench.context();
    return { target: c['edit.target'], canUndo: c['edit.canUndo'], canRedo: c['edit.canRedo'], undo: c['edit.undoTitle'] };
  });
const stateText = async () => ((await page.locator('#stat-state').textContent().catch(() => '')) ?? '').trim();

// The mean colour of a canvas strip as composited (a presented WebGPU canvas cannot be read with drawImage).
async function stripColour(label) {
  const box = await page.locator('#canvas').boundingBox();
  const png = await page.screenshot({ clip: { x: box.x + 4, y: box.y + 26, width: 110, height: 28 } });
  return page.evaluate(async (data) => {
    const bytes = Uint8Array.from(atob(data), (c) => c.charCodeAt(0));
    const bitmap = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
    const scratch = document.createElement('canvas');
    scratch.width = 8;
    scratch.height = 8;
    const ctx = scratch.getContext('2d');
    ctx.drawImage(bitmap, 0, 0, 8, 8);
    const px = ctx.getImageData(0, 0, 8, 8).data;
    let r = 0;
    let g = 0;
    let b = 0;
    for (let i = 0; i < px.length; i += 4) {
      r += px[i];
      g += px[i + 1];
      b += px[i + 2];
    }
    const n = px.length / 4;
    return { r: Math.round(r / n), g: Math.round(g / n), b: Math.round(b / n) };
  }, png.toString('base64'));
}
const isRed = (c) => c.r > 200 && c.g < 90 && c.b < 90;

await page.goto(base, { waitUntil: 'load' });
await page.getByRole('button', { name: /Counter/ }).first().click();
await page.waitForFunction(() => /drew/.test(document.querySelector('#canvas-status')?.textContent ?? ''), null, { timeout: 60000 });
const box = await page.locator('#canvas').boundingBox();

// Give the app some state, so "no full re-init" is observable.
await page.mouse.click(box.x + 40, box.y + 34);
await page.waitForTimeout(500);
const stateBefore = await stateText();
check('the app has state before any override', /[1-9]/.test(stateBefore), stateBefore);

// Select the Button with the inspector.
await page.locator('#inspect-toggle').click();
await page.mouse.click(box.x + 40, box.y + 34);
await page.waitForSelector('.inspect-heading');
check('the inspector names the node', /^Button #/.test((await page.locator('.inspect-heading').textContent()) ?? ''));
check('authored values are not marked as overrides', (await page.locator('.is-overridden').count()) === 0);
const authored = await stripColour('authored');
check('the button is not red before the override', !isRed(authored), JSON.stringify(authored));

// Refused value: the diagnostic code shows inline and nothing changes.
await page.fill('.inspect-override-form [data-field="property"]', 'colour');
await page.fill('.inspect-override-form [data-field="value"]', 'red');
await page.press('.inspect-override-form [data-field="value"]', 'Enter');
await page.waitForSelector('.inspect-diagnostic');
check('a refused value shows its TESSERA_CSS code', /TESSERA_CSS_/.test((await page.locator('.inspect-diagnostic').first().textContent()) ?? ''));
check('a refusal leaves no override', (await page.locator('.inspect-override').count()) === 0);

// Set a colour with the swatch: one override, marked, and the frame repaints.
await page.fill('input[data-field="swatch:background-color"]', '#ff0000');
await page.waitForSelector('.inspect-override[data-property="background-color"]');
await page.waitForTimeout(600);
check('the override is listed and badged', (await page.locator('.inspect-override .inspect-value').first().textContent()) === '#ff0000' && (await page.locator('.inspect-badge').count()) >= 1);
check('the frame repainted red', isRed(await stripColour('override')));
check('state survived the override', (await stateText()) === stateBefore);

// Undo / redo routing.
await page.locator('.inspect-revert').focus();
let keys = await editKeys();
check('focus in the inspector targets the override stack', keys.target === 'override' && keys.canUndo && /Set background-color on Button/.test(keys.undo), JSON.stringify(keys));
await page.keyboard.press('Control+z');
await page.waitForTimeout(500);
check('Ctrl+Z on the inspector reverts the override and the frame', (await page.locator('.inspect-override').count()) === 0 && !isRed(await stripColour('undone')));
await page.locator('.inspect-revert').focus();
await page.keyboard.press('Control+y');
await page.waitForTimeout(500);
check('Ctrl+Y brings it back', (await page.locator('.inspect-override').count()) === 1 && isRed(await stripColour('redone')));
await page.locator('.inspect-override-form [data-field="property"]').focus();
keys = await editKeys();
check('a text field keeps the browser undo (target native)', keys.target === 'native', JSON.stringify(keys));
await page.keyboard.type('abc');
await page.keyboard.press('Control+z');
check('native undo changed the field, not the override stack', (await page.locator('.inspect-override').count()) === 1);

// Commit inline: the file text changes, the frame stays red, the overrides settle, and the app is not re-initialised.
await page.locator('.inspect-commit-button[data-destination="inline"]').click();
await page.waitForSelector('.inspect-commit-answer[data-status]', { timeout: 15000 });
const status = await page.locator('.inspect-commit-answer').getAttribute('data-status');
check('the commit round trip settled', status === 'committed', status + ' ' + (await page.locator('.inspect-commit-answer').textContent()));
const committed = await editorText();
check('the file text changed', /style=/.test(committed) && /#ff0000/i.test(committed));
await page.waitForTimeout(600);
check('no overrides remain after the settle', (await page.locator('.inspect-override').count()) === 0);
check('the frame is red from the source now', isRed(await stripColour('committed')));
check('state survived the commit (no full re-init)', (await stateText()) === stateBefore);

// The text history holds ONE entry for the commit; the override stack holds nothing of that node.
await page.locator('#source').click();
keys = await editKeys();
check('the Edit menu names the commit in the editor', keys.target === 'text' && /Commit style to Button/.test(keys.undo), JSON.stringify(keys));
await page.keyboard.press('Control+z');
await page.waitForTimeout(800);
check('one Ctrl+Z in the editor reverts the commit', !/style=/.test(await editorText()));
check('and the frame follows the source', !isRed(await stripColour('reverted')));

// A stale buffer is rejected: type, then commit before the preview has evaluated the keystroke.
await page.mouse.click(box.x + 40, box.y + 34);
await page.waitForSelector('.inspect-heading');
await page.fill('input[data-field="swatch:background-color"]', '#00aa00');
await page.waitForSelector('.inspect-override[data-property="background-color"]');
const before = await editorText();
await page.evaluate(() => {
  window.__tesseraEditor.edit(window.__tesseraEditor.text() + '\n');
  document.querySelector('.inspect-commit-button[data-destination="inline"]').click();
});
// The keystroke's evaluation re-draws the tree, which may re-ask and hide the panel, so the answer is read
// from the DOM whether or not it is still visible.
await page.waitForSelector('.inspect-commit-answer[data-status="rejected"]', { state: 'attached', timeout: 15000 });
const rejected = await page.locator('.inspect-commit-answer').getAttribute('data-status');
check('a stale buffer is rejected (version_mismatch)', rejected === 'rejected', await page.locator('.inspect-commit-answer').textContent());
check('a rejected patch left the text as typed', (await editorText()) === before + '\n');

// The typed edit restarts a sample project (a sample file is not a `source` kind for the live edit, which is the
// page's existing rule), and a restart ends the session and its overrides, so the screenshots start a new one.
await page.waitForTimeout(1500);
await page.mouse.click(box.x + 40, box.y + 34);
await page.waitForSelector('.inspect-heading');
await page.fill('input[data-field="swatch:background-color"]', '#00aa00');
await page.waitForSelector('.inspect-override[data-property="background-color"]');

// Screenshots of the inspector with an override in light, dark and 390 px.
// Last, because the 390 px phone layout collapses the dock and releases the canvas session.
if (shots) {
  const panel = () => page.locator('#inspect-panel');
  await page.evaluate(() => (document.documentElement.dataset.theme = 'light'));
  await panel().screenshot({ path: join(shots, 'inspector-light.png') });
  await page.evaluate(() => (document.documentElement.dataset.theme = 'dark'));
  await panel().screenshot({ path: join(shots, 'inspector-dark.png') });
  await page.evaluate(() => (document.documentElement.dataset.theme = 'light'));
  await page.setViewportSize({ width: 390, height: 844 });
  await page.waitForTimeout(500);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth);
  check('390 px: no horizontal page scroll', !overflow);
  // The phone layout shows one panel at a time: open Canvas, select the Button, and set an override.
  await page.getByRole('tab', { name: /Canvas/ }).first().click();
  await page.waitForFunction(() => /drew/.test(document.querySelector('#canvas-status')?.textContent ?? ''), null, { timeout: 60000 });
  await page.waitForTimeout(500);
  const phone = await page.locator('#canvas').boundingBox();
  if ((await page.locator('#inspect-toggle').getAttribute('aria-pressed')) !== 'true') await page.locator('#inspect-toggle').click();
  await page.mouse.click(phone.x + 40, phone.y + 34);
  await page.waitForSelector('.inspect-heading');
  await page.fill('input[data-field="swatch:background-color"]', '#00aa00');
  await page.waitForSelector('.inspect-override');
  await page.locator('#inspect-panel').scrollIntoViewIfNeeded();
  check('390 px: the inspector fits without horizontal scroll', !(await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth || document.querySelector('#inspect-panel').scrollWidth > document.querySelector('#inspect-panel').clientWidth + 1)));
  await page.screenshot({ path: join(shots, 'inspector-390.png') });
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.waitForTimeout(500);
}

check('no page errors', errors.length === 0, errors.slice(0, 3).join(' | '));
console.log(failures === 0 ? '\nall checks passed' : `\n${failures} check(s) failed`);
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
