// Issue #390: prove in a real browser that the author stylesheet changes what the canvas paints, and
// that an empty sheet still paints the built-in look.
//
// **Why the colour comes from a screenshot and not `drawImage`.** A WebGPU canvas whose frame has been
// presented cannot be read back with `drawImage`/`getImageData`: the swapchain texture is already
// consumed, so every read returns transparent black. That is a property of the platform, not of the
// preview. Playwright's element screenshot captures the *composited* page, so the pixels it contains are
// the pixels a person sees -- which is exactly the evidence this probe is for. The PNG is decoded by the
// browser itself (`createImageBitmap`), so no image library is needed here.
//
// Not a repository test: run it from a directory whose node_modules has playwright.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { chromium } from 'playwright';
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
await page.goto(base, { waitUntil: 'load' });

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
}

async function status() {
  return (await page.locator('#canvas-status').textContent())?.trim();
}

// The average colour of the canvas element as the page composites it, decoded from a real screenshot.
async function canvasColour(label) {
  const png = await page.locator('#canvas').screenshot({ path: `${outDir}/390-${label}.png` });
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

function describe(colour) {
  return `rgb(${colour.r}, ${colour.g}, ${colour.b})`;
}

const SOURCE = `export function Screen(): Node {
  return (
    <View className="page">
      <Text className="title">Author CSS</Text>
      <Button>Go</Button>
    </View>
  );
}
`;

await page.locator('#tab-canvas').click();
await page.waitForTimeout(600);

// --- case 0: the seeded project's sheet is imported by the entry, so it is real author CSS -----------
// The studio seeds `styles/theme.css` with a rule and `ui/App.tsx` imports it; a sheet that is imported
// and non-empty applies. (The built-in look is used only when no imported sheet carries any CSS: case 1.)

// --- case 1: no author CSS -> the built-in fallback paints ----------------------------------------
await editEntry(page, SOURCE);
await editSheet(page, '');
await page.locator('#restart-app').click();
await page.waitForTimeout(2500);
check('empty sheet: canvas draws', /draw call/.test((await status()) ?? ''), await status());
const fallback = await canvasColour('fallback');
check(
  'empty sheet: paints the built-in dark surface (blue-dominant)',
  fallback.b > fallback.r + 8,
  describe(fallback),
);

// --- case 2: author CSS paints something else -----------------------------------------------------
const RED_CSS = `.page { display: flex; flex-direction: column; background-color: rgb(220, 30, 30); width: 320px; height: 240px; }
.title { color: rgb(255, 255, 255); height: 24px; font-size: 16px; }
Button { width: 300px; height: 200px; background-color: rgb(240, 20, 20); }
`;
await editSheet(page, RED_CSS);
await page.locator('#restart-app').click();
await page.waitForTimeout(2500);
check('author CSS: canvas draws', /draw call/.test((await status()) ?? ''), await status());
const authored = await canvasColour('author');
check(
  'author CSS: the frame is visibly red, not the built-in blue',
  authored.r > authored.b + 60,
  describe(authored),
);
check(
  'author CSS: the colour differs from the fallback',
  Math.abs(authored.r - fallback.r) > 40,
  `${describe(fallback)} -> ${describe(authored)}`,
);

// --- case 3: a :root token through var() changes the drawn colour ---------------------------------
await editSheet(
  page,
  ':root { --brand: rgb(20, 200, 60); }\n' +
    '.page { display: flex; flex-direction: column; background-color: var(--brand); width: 320px; height: 240px; }\n' +
    '.title { height: 24px; color: rgb(0, 0, 0); }\n',
);
await page.locator('#restart-app').click();
await page.waitForTimeout(2500);
const tokened = await canvasColour('token');
check(
  'a :root token through var() changes the drawn colour to green',
  tokened.g > tokened.r + 30 && tokened.g > tokened.b + 20,
  describe(tokened),
);

// --- case 4: a bad rule is reported with line/column and does NOT blank the frame ------------------
await editSheet(
  page,
  '.page { display: flex; flex-direction: column; background-color: rgb(210, 40, 40); '
    + 'width: 320px; height: 240px; }\n'
    + '.title { flx-grow: 1; height: 24px; }\n',
);
await page.locator('#restart-app').click();
await page.waitForTimeout(2500);
const failedBody = (await page.locator('#failed-body').textContent())?.trim() ?? '';
check(
  'a bad rule is reported with file:line:column and the code',
  /styles\/theme\.css:2:\d+: TESSERA_CSS_UNKNOWN_PROPERTY/.test(failedBody),
  failedBody.split('\n')[0] ?? failedBody,
);
const broken = await canvasColour('badrule');
check(
  'a bad rule does NOT blank the frame (it still paints red)',
  broken.r > broken.b + 60,
  describe(broken),
);

// --- case 5: emptying the sheet returns to the built-in look --------------------------------------
await editSheet(page, '');
await page.locator('#restart-app').click();
await page.waitForTimeout(2500);
const restored = await canvasColour('restored');
check(
  'emptying the sheet restores the built-in look',
  restored.b > restored.r + 8,
  describe(restored),
);

check('No console errors', consoleErrors.length === 0, consoleErrors.slice(0, 3).join(' | ') || 'none');

console.log(`\n${failures === 0 ? 'all author-CSS checks passed' : `${failures} check(s) failed`}`);
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
