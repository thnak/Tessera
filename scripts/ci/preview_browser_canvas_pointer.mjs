// SPDX-License-Identifier: MIT
//
// Browser acceptance for #388 (the page half): **pointer input on the canvas**. Moving over a Button shows
// its `:hover` look, pressing shows `:active`, releasing over it runs its `onClick`, and the state change is
// drawn on the next frame -- with every step one on-demand frame and a static scene presenting nothing.
//
//   node scripts/ci/preview_browser_canvas_pointer.mjs [site-dir]
//
// The look comes from the author's stylesheet (`Button:hover`, `Button:active`), so a pass proves the
// engine's style matcher saw the pseudo-state the router set -- no class name is written by the page. The
// colour is read from a real screenshot of the canvas element, because a presented WebGPU frame cannot be
// read back with `drawImage` (the same technique, and reason, as `preview_browser_css_live_edit.mjs`).
//
// Not a repository test: run it from a directory whose node_modules has playwright.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { chromium } from 'playwright';
import { editEntry, editSheet, tab, tabButton } from './studio_project.mjs';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.ttf': 'font/ttf',
  '.txt': 'text/plain; charset=utf-8',
};
const site = process.argv[2] ?? 'build/pages';
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
    response.writeHead(404);
    response.end();
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch({
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader'],
});
const page = await (await browser.newContext()).newPage();
const consoleErrors = [];
page.on('console', (message) => {
  if (message.type() === 'error') consoleErrors.push(message.text());
});
page.on('pageerror', (error) => consoleErrors.push(`pageerror: ${String(error)}`));
await page.goto(base, { waitUntil: 'load' });

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
}

const settle = (ms = 600) => page.waitForTimeout(ms);

// The average colour of the canvas element as the page composites it.
async function canvasColour(label) {
  const png = await page.locator('#canvas').screenshot();
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
  }, png.toString('base64'));
}
const hue = (c) => (c.r > c.g + 60 && c.r > c.b + 60 ? 'red' : c.g > c.r + 40 && c.g > c.b + 40 ? 'green' : c.b > c.r + 60 && c.b > c.g + 60 ? 'blue' : c.r > 150 && c.g > 120 && c.b < 90 ? 'yellow' : `other(${c.r},${c.g},${c.b})`);

const SOURCE = `export function Screen(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View className="page">
      <Button onClick={() => setCount(count + 1)}>Add {count}</Button>
    </View>
  );
}
`;
// Green at rest, red on hover, blue while pressed: three hues the built-in blue sheet cannot produce.
const CSS = `.page { display: flex; flex-direction: column; background-color: rgb(30, 30, 40); width: 320px; height: 240px; }
Button { width: 300px; height: 200px; background-color: rgb(20, 160, 60); }
Button:hover { background-color: rgb(220, 30, 30); }
Button:active { background-color: rgb(30, 30, 220); }
Button:focus-visible { background-color: rgb(250, 200, 20); }
`;

await tab(page, 'canvas');
await page.waitForTimeout(600);
await editEntry(page, SOURCE);
await editSheet(page, CSS);
await page.locator('#restart-app').click();
await settle(2500);

// The canvas' CSS box and the logical viewport it shows, so a point can be placed on the Button.
const geometry = await page.evaluate(() => {
  const canvas = document.getElementById('canvas');
  const box = canvas.getBoundingClientRect();
  return {
    left: box.left + canvas.clientLeft,
    top: box.top + canvas.clientTop,
    width: canvas.clientWidth,
    height: canvas.clientHeight,
    logicalWidth: canvas.width / devicePixelRatio,
    logicalHeight: canvas.height / devicePixelRatio,
  };
});
const at = (logicalX, logicalY) => ({
  x: geometry.left + (logicalX * geometry.width) / geometry.logicalWidth,
  y: geometry.top + (logicalY * geometry.height) / geometry.logicalHeight,
});
const onButton = at(100, 100);
const stateText = async () => ((await page.locator('#stat-state').textContent()) ?? '').trim();
const frames = async () => ((await page.locator('#stat-frame').textContent()) ?? '').trim();

check('at rest the Button is its own colour (green)', hue(await canvasColour('rest')) === 'green', hue(await canvasColour('rest')));
check('the state starts at 0', /count=0/.test(await stateText()), await stateText());

// --- hover ---------------------------------------------------------------------------------------------
await page.mouse.move(onButton.x, onButton.y);
await settle();
check('moving over the Button shows its :hover look (red)', hue(await canvasColour('hover')) === 'red', hue(await canvasColour('hover')));

// --- active --------------------------------------------------------------------------------------------
await page.mouse.down();
await settle();
check('pressing shows its :active look (blue)', hue(await canvasColour('active')) === 'blue', hue(await canvasColour('active')));
check('a press alone has not run onClick yet', /count=0/.test(await stateText()), await stateText());

// --- click -------------------------------------------------------------------------------------------------
await page.mouse.up();
await settle();
check('releasing over it runs onClick and the state is drawn (count=1)', /count=1/.test(await stateText()), await stateText());
check('after the release the pointer is still over it: hover again (red)', hue(await canvasColour('after-click')) === 'red', hue(await canvasColour('after-click')));

// --- leaving clears the hover --------------------------------------------------------------------------
await page.mouse.move(geometry.left - 40, geometry.top + 10);
await settle();
check('moving off the canvas clears the hover (green)', hue(await canvasColour('left')) === 'green', hue(await canvasColour('left')));

// --- a press dragged off the Button does not click ---------------------------------------------------
await page.mouse.move(onButton.x, onButton.y);
await page.mouse.down();
await page.mouse.move(geometry.left + geometry.width - 2, geometry.top + geometry.height - 2);
await page.mouse.up();
await settle();
check('release away from the Button runs no onClick (count stays 1)', /count=1/.test(await stateText()), await stateText());

// --- one frame per change, none for a static scene ---------------------------------------------------
await page.mouse.move(geometry.left - 40, geometry.top + 10);
await settle();
const idleBefore = await frames();
await settle(900);
check('a static scene presents nothing: the frame counter does not move', (await frames()) === idleBefore, `${idleBefore} -> ${await frames()}`);
await page.mouse.move(onButton.x, onButton.y);
await settle();
const afterHover = await frames();
await page.mouse.move(onButton.x + 3, onButton.y + 3);
await settle();
check('a move inside the same element changes nothing, so it paints nothing', (await frames()) === afterHover, `${afterHover} -> ${await frames()}`);

// --- keyboard focus (#388) ---------------------------------------------------------------------------
// Move the mouse away so hover is not in play, then arrive at the canvas with the keyboard.
await page.mouse.move(geometry.left - 40, geometry.top + 10);
await settle();
await tabButton(page, 'canvas').focus();
for (let step = 0; step < 12; step += 1) {
  if (await page.evaluate(() => document.activeElement?.id === 'canvas')) break;
  await page.keyboard.press('Tab');
}
await settle();
check('Tab reaches the canvas as a focus stop', await page.evaluate(() => document.activeElement?.id === 'canvas'));
check('arriving by keyboard focuses the first control and shows its :focus-visible look (yellow)', hue(await canvasColour('focus')) === 'yellow', hue(await canvasColour('focus')));

const before = await stateText();
await page.keyboard.press('Enter');
await settle();
const afterEnter = await stateText();
check('Enter activates the focused Button', Number(/count=(\d+)/.exec(afterEnter)?.[1]) === Number(/count=(\d+)/.exec(before)?.[1]) + 1, `${before} -> ${afterEnter}`);
await page.keyboard.press(' ');
await settle();
const afterSpace = await stateText();
check('Space activates it too', Number(/count=(\d+)/.exec(afterSpace)?.[1]) === Number(/count=(\d+)/.exec(afterEnter)?.[1]) + 1, `${afterEnter} -> ${afterSpace}`);
check('the ring is still showing after activation', hue(await canvasColour('focus-after')) === 'yellow', hue(await canvasColour('focus-after')));

// Tab at the end of the order lets go: the browser carries on, and the canvas stops looking focused.
await page.keyboard.press('Tab');
await settle();
check('Tab at the end of the controls leaves the canvas (no keyboard trap)', await page.evaluate(() => document.activeElement?.id !== 'canvas'), await page.evaluate(() => document.activeElement?.id || document.activeElement?.tagName));
check('and the ring is gone (green)', hue(await canvasColour('focus-left')) === 'green', hue(await canvasColour('focus-left')));
await page.keyboard.press('Shift+Tab');
await settle();
check('Shift+Tab comes back to the canvas and focuses its control (yellow)', (await page.evaluate(() => document.activeElement?.id === 'canvas')) && hue(await canvasColour('focus-back')) === 'yellow', hue(await canvasColour('focus-back')));

// A key with a modifier is not the scene's: Ctrl+Enter must not click.
const beforeChord = await stateText();
await page.keyboard.press('Control+Enter');
await settle();
check('a chord with Ctrl is left to the page (no click)', (await stateText()) === beforeChord, `${beforeChord} -> ${await stateText()}`);

// --- resize and fold while the app keeps its state ---------------------------------------------------
const counted = await stateText();
const widthBefore = await page.evaluate(() => document.getElementById('canvas').width);
await page.evaluate(() => window.__tesseraWorkbench.run('device.setViewport', { value: '768x1024' }));
await settle(900);
const widthAfter = await page.evaluate(() => document.getElementById('canvas').width);
check('the canvas resizes to the new device', widthAfter !== widthBefore, `${widthBefore} -> ${widthAfter}`);
check('the app keeps its state across the resize (no re-evaluation)', (await stateText()) === counted, `${counted} -> ${await stateText()}`);
await page.evaluate(() => window.__tesseraWorkbench.run('device.setFold', { value: 'book' }));
await settle(900);
check('the app keeps its state across a fold posture change', (await stateText()) === counted, `${counted} -> ${await stateText()}`);
await page.evaluate(() => window.__tesseraWorkbench.run('device.setFold', { value: 'none' }));
await settle(600);
check('and across unfolding', (await stateText()) === counted, `${counted} -> ${await stateText()}`);

check(
  'No console errors',
  consoleErrors.filter((entry) => !/Failed to load resource/.test(entry)).length === 0,
  consoleErrors.slice(0, 3).join(' | ') || 'none',
);
console.log(`\n${failures === 0 ? 'all canvas pointer checks passed' : `${failures} check(s) failed`}`);
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
