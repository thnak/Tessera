// The combined acceptance for #390 x #444: **the author's stylesheet survives a live edit**.
//
// Each feature's own probe stops short of this. `author-css-390.mjs` changes the sheet and presses a
// button (a cold start, `#restart-app` after #444's rename); `preview_browser_live_edit.mjs` types into
// the source with the page's shipped comment-only sheet, so the built-in look is in force and a
// stylesheet regression on the edit path would be invisible to both.
//
// The failure this probe exists to catch is specific and silent: #444 replaced the stateless
// `tessera_canvas_frame(source, ..., stylesheet, ...)` with `tessera_canvas_apply(...)` then
// `tessera_canvas_draw()`. The sheet argument belongs on the **apply** (the call that re-evaluates);
// `draw` takes no strings. If the merge had put it on `draw` -- or dropped it from `apply` -- every
// subsequent keystroke would re-evaluate the source against the built-in sheet and repaint in the
// engine's default blue, with no error anywhere. So the assertion is a *colour read after a keystroke*,
// not a status string: the canvas must still be the author's red.
//
// **Why the colour comes from a screenshot and not `drawImage`.** A WebGPU canvas whose frame has been
// presented cannot be read back via `drawImage`/`getImageData` (the swapchain texture is consumed, so
// every read is transparent black). Playwright's element screenshot captures the composited page, which
// is the pixel a person sees. Same technique, and same reason, as `author-css-390.mjs`.
//
// Not a repository test: run it from a directory whose node_modules has playwright.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { chromium } from 'playwright';
import { ENTRY, editEntry, editSheet, selectFile } from './studio_project.mjs';

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

const site = process.argv[2] ?? 'build/pages';
const outDir = process.argv[3] ?? 'build';

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

const status = async () => (await page.locator('#canvas-status').textContent())?.trim();

// The average colour of the canvas element as the page composites it, decoded from a real screenshot.
async function canvasColour(label) {
  const png = await page.locator('#canvas').screenshot({ path: `${outDir}/combined-${label}.png` });
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

// The debounce (150 ms) plus the apply and a frame, with margin. Same value the live-edit probe uses.
const settle = (extra = 900) => page.waitForTimeout(extra);

// Types one character at the caret and fires `input`, exactly as `preview_browser_live_edit.mjs` does:
// the page's live-edit trigger listens on `input`, not on a key event, so this is the hot path.
async function typeCharacter(character) {
  await page.evaluate((text) => {
    const node = document.getElementById('source');
    const start = node.selectionStart ?? node.value.length;
    const end = node.selectionEnd ?? start;
    node.value = node.value.slice(0, start) + text + node.value.slice(end);
    node.selectionStart = node.selectionEnd = start + text.length;
    node.dispatchEvent(new Event('input', { bubbles: true }));
  }, character);
}

// Replaces `from` with `to` inside the source buffer and fires `input`. Every edit below goes through
// this rather than appending at the end: the source is a whole module, so a character after the closing
// brace would stop it parsing and the step would test the error path instead of the apply path.
async function editSource(from, to) {
  const changed = await page.evaluate(
    ([a, b]) => {
      const node = document.getElementById('source');
      if (!node.value.includes(a)) {
        return false;
      }
      node.value = node.value.replace(a, b);
      node.focus();
      node.selectionStart = node.selectionEnd = node.value.length;
      node.dispatchEvent(new Event('input', { bubbles: true }));
      return true;
    },
    [from, to],
  );
  if (!changed) {
    throw new Error(`the source did not contain ${JSON.stringify(from)}`);
  }
}

const SOURCE = `export function Screen(): Node {
  const [count, setCount] = useState<int>(5);
  return (
    <View className="page">
      <Text className="title">Author CSS {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Go</Button>
    </View>
  );
}
`;

// The same sheet `author-css-390.mjs` calls case 2: an unmistakable red that no built-in default can
// produce (the engine's fallback measures rgb(18, 31, 53), blue-dominant).
const RED_CSS = `.page { display: flex; flex-direction: column; background-color: rgb(220, 30, 30); width: 320px; height: 240px; }
.title { color: rgb(255, 255, 255); height: 24px; font-size: 16px; }
Button { width: 300px; height: 200px; background-color: rgb(240, 20, 20); }
`;

await page.locator('#tab-canvas').click();
await page.waitForTimeout(600);

// --- 1. Apply author CSS through the cold start, and confirm it paints -------------------------------
await editEntry(page, SOURCE);
await editSheet(page, RED_CSS);
await page.locator('#restart-app').click();
await settle(2500);
check('author CSS applied: the canvas draws', /draw call/.test((await status()) ?? ''), await status());
const before = await canvasColour('before-edit');
check(
  'author CSS applied: the frame is the author red, not the built-in blue',
  before.r > before.b + 60,
  describe(before),
);

// --- 2. The live edit: a source change with no button -------------------------------------------
// The edit changes the **initializer** and the text a person reads, which is a real re-evaluation.
// Changing the initializer is #444's own sharper test: a re-initialisation would visibly reset the
// cell, so the two facts below cannot both be satisfied by a cold start.
//
// It must NOT be a restart: the apply path is the only path #390's sheet now travels on.
const beforeEditState = (
  await page.evaluate(() => document.getElementById('stat-state')?.textContent ?? '')
).trim();
check(
  'the state row exists, so a carried cell is observable on the canvas pane',
  beforeEditState.length > 0,
  beforeEditState || '(no cells)',
);
const initial = /count=(-?\d+)/.exec(beforeEditState);
check(
  'a fresh document starts at its initializer (5)',
  initial !== null && Number(initial[1]) === 5,
  beforeEditState,
);

// The editor shows the stylesheet after the edit above; the live edit is a source edit.
await selectFile(page, ENTRY);
await page.evaluate(() => {
  const node = document.getElementById('source');
  node.value = node.value
    .replace('useState<int>(5)', 'useState<int>(0)')
    .replace('Author CSS', 'Author CSS!');
  node.focus();
  node.selectionStart = node.selectionEnd = node.value.length;
  node.dispatchEvent(new Event('input', { bubbles: true }));
});
await settle();

const afterEdit = await canvasColour('after-live-edit');
check(
  'the live edit kept the author stylesheet (the frame is still the author red)',
  afterEdit.r > afterEdit.b + 60,
  `${describe(before)} -> ${describe(afterEdit)}`,
);
check(
  'the live edit did not fall back to the built-in blue',
  !(afterEdit.b > afterEdit.r + 8),
  describe(afterEdit),
);

const stateAfterEdit = (
  await page.evaluate(() => document.getElementById('stat-state')?.textContent ?? '')
).trim();
const cellValue = (text) => {
  const match = /count=(-?\d+)/.exec(text);
  return match ? Number(match[1]) : null;
};
check(
  'the live edit carried the count (a restart would have re-initialised it to 0)',
  cellValue(stateAfterEdit) === 5,
  `${beforeEditState} -> ${stateAfterEdit}`,
);

// --- 4. The same edit, one more burst: the sheet still survives a second apply ----------------------
await editSource('Author CSS!', 'Author CSS!!');
await settle();
const afterSecond = await canvasColour('after-second-edit');
check(
  'a second live edit still paints the author red',
  afterSecond.r > afterSecond.b + 60,
  `${describe(afterEdit)} -> ${describe(afterSecond)}`,
);

// --- 5. A stylesheet edited live also reaches the canvas ------------------------------------------
// This is the other direction of the same union: the sheet is an input to the apply, so changing it
// and then making one source edit -- no button -- must repaint in the new colour, and the sheet is part
// of the session's reuse tuple, so the new text re-resolves style rather than reusing the old cascade.
const GREEN_CSS = `.page { display: flex; flex-direction: column; background-color: rgb(20, 160, 60); width: 320px; height: 240px; }
.title { color: rgb(0, 0, 0); height: 24px; }
`;
await editSheet(page, GREEN_CSS);
await selectFile(page, ENTRY);
// A real text edit inside the source, not an appended character: appending to the end of the module
// leaves the closing brace after the new text and the source stops parsing, which is a different test.
await editSource('Author CSS!!', 'Author CSS day');
await settle();
const afterCssEdit = await canvasColour('after-css-edit');
check(
  'a stylesheet changed without pressing Restart reaches the canvas on the live apply',
  afterCssEdit.g > afterCssEdit.r + 40 && afterCssEdit.g > afterCssEdit.b + 40,
  describe(afterCssEdit),
);

check('No console errors', consoleErrors.length === 0, consoleErrors.slice(0, 3).join(' | ') || 'none');

console.log(
  `\n${failures === 0 ? 'all combined author-CSS x live-edit checks passed' : `${failures} check(s) failed`}`,
);
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
