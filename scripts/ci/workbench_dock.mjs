#!/usr/bin/env node
/**
 * The studio dock's browser acceptance (issue #516, implemented by W5 #560; `studio-workbench-layout-v1.md`
 * sections 3, 4 and 5): real Chromium, **real mouse drags** (mouse down, many moves, mouse up; never a
 * synthetic DragEvent) and keyboard-only docking.
 *
 * Developer-run, not a CI gate: it needs a browser (and a WebGPU-capable one for the engine's own frames).
 * The pure parts run in Node as `workbench_layout.mjs` and `workbench_dock_core.mjs`.
 *
 * Usage:
 *   node scripts/ci/workbench_dock.mjs [--site designer/web/dist | --url http://127.0.0.1:5173/] [--shots dir]
 * `playwright` is not a repository dependency: set PLAYWRIGHT_DIR to a directory whose node_modules holds it,
 * and PLAYWRIGHT_CHROMIUM to a chrome executable if the bundled revision is not installed.
 *
 * What it checks (1440x900 unless said): the default layout (Project | Editor | Canvas, the panel area
 * collapsed so no Diagnostics box exists, the status-bar Diagnostics toggle), Ctrl+J, a tab switch moving
 * nothing else, open/close of Canvas/Scene/Diagnostics independently (closing Canvas leaves no GPU device
 * taken), a real drag of Canvas to the bottom and of Diagnostics to the right with the Canvas element
 * identical and never removed, keyboard moves (Alt+Shift+Arrow) with focus return and an aria-live message,
 * reload persistence, a throwing localStorage, a newer-version layout, ARIA and splitter keys, maximize and
 * restore, Reset layout not remounting an unchanged panel, no WebGPU keeping Scene and Diagnostics, and the
 * 390 px phone collapse (one tab strip, no horizontal scroll, desktop layout in storage untouched).
 */
import { createServer } from 'node:http';
import { mkdir, readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { extname, join, normalize, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const args = process.argv.slice(2);
const arg = (name, fallback) => (args.includes(name) ? args[args.indexOf(name) + 1] : fallback);
const shots = arg('--shots', null);
const site = arg('--site', 'designer/web/dist');
let base = arg('--url', null);

async function loadPlaywright() {
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

const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.wasm': 'application/wasm', '.txt': 'text/plain', '.ttf': 'font/ttf' };
let server = null;
if (base === null) {
  server = createServer(async (request, response) => {
    let path = normalize(decodeURIComponent(new URL(request.url, 'http://x').pathname));
    if (path === '/' || path === '\\') path = '/index.html';
    try {
      const body = await readFile(join(site, path));
      response.writeHead(200, { 'content-type': MIME[extname(path)] ?? 'application/octet-stream', 'cache-control': 'no-store' });
      response.end(body);
    } catch {
      response.writeHead(404);
      response.end('not found');
    }
  });
  await new Promise((done) => server.listen(0, '127.0.0.1', done));
  base = `http://127.0.0.1:${server.address().port}/`;
}
console.log(`testing ${base}`);

const chromium = await loadPlaywright();
const browser = await chromium.launch({
  executablePath: process.env.PLAYWRIGHT_CHROMIUM || undefined,
  args: ['--enable-unsafe-webgpu', '--use-angle=swiftshader'],
});
let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok   ' : 'FAIL '} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
};
const snap = async (page, name) => {
  if (shots) {
    await mkdir(shots, { recursive: true });
    await page.screenshot({ path: join(shots, `${name}.png`) });
  }
};

/** A page with a fresh storage, the console errors it produced (a missing Wasm 404 is not one), and a probe. */
async function open(width, height, { init, search = '', storage } = {}) {
  const context = await browser.newContext({ viewport: { width, height } });
  if (init) await context.addInitScript(init);
  if (storage) await context.addInitScript((entries) => { for (const [k, v] of Object.entries(entries)) localStorage.setItem(k, v); }, storage);
  const page = await context.newPage();
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error' && !/404/.test(m.text())) errors.push(m.text().slice(0, 200)); });
  page.on('pageerror', (e) => errors.push(`pageerror: ${String(e).slice(0, 300)}`));
  await page.goto(`${base}${search}`, { waitUntil: 'load' });
  await page.waitForSelector('.flexlayout__layout', { timeout: 15000 });
  await page.waitForTimeout(800);
  await page.evaluate(() => {
    window.__probe = { removals: 0 };
    window.__probe.canvas = document.getElementById('canvas');
    new MutationObserver((records) => {
      for (const r of records) for (const n of r.removedNodes) {
        if (window.__probe.canvas && (n === window.__probe.canvas || (n.contains && n.contains(window.__probe.canvas)))) window.__probe.removals += 1;
      }
    }).observe(document.documentElement, { childList: true, subtree: true });
  });
  return { page, errors, context };
}

const box = (page, selector) =>
  page.evaluate((s) => {
    const node = document.querySelector(s);
    if (!node || !node.isConnected) return null;
    const r = node.getBoundingClientRect();
    return { x: Math.round(r.x), y: Math.round(r.y), w: Math.round(r.width), h: Math.round(r.height) };
  }, selector);
const model = (page) => page.evaluate(() => window.__tesseraWorkbench.layout());
const sameCanvas = (page) => page.evaluate(() => document.getElementById('canvas') === window.__probe.canvas && window.__probe.canvas !== null);
const removals = (page) => page.evaluate(() => window.__probe.removals);
const groupOf = (layout, id) => {
  const walk = (node) => (node.type === 'group' ? (node.panels.includes(id) ? node : null) : node.children.map(walk).find(Boolean) ?? null);
  return walk(layout.root);
};
const via = (page, source) => page.evaluate((s) => new Function('wb', `return (${s})`)(window.__tesseraWorkbench), source);

async function drag(page, from, to) {
  await page.mouse.move(from.x, from.y);
  await page.mouse.down();
  for (let i = 1; i <= 30; i += 1) {
    await page.mouse.move(from.x + ((to.x - from.x) * i) / 30, from.y + ((to.y - from.y) * i) / 30);
  }
  await page.waitForTimeout(450);
  await page.mouse.up();
  await page.waitForTimeout(450);
}
const centre = (b) => ({ x: b.x + b.w / 2, y: b.y + b.h / 2 });

// ---- 1. default layout, the collapsed panel area, the status-bar toggle, Ctrl+J, tab switching -------------

{
  const { page, errors, context } = await open(1440, 900);
  const project = await box(page, '[data-panel=project]');
  const editor = await box(page, '[data-panel=editor]');
  const canvas = await box(page, '[data-panel=canvas]');
  check('default: Project | Editor | Canvas side by side, in that order',
    Boolean(project && editor && canvas) && project.x < editor.x && editor.x < canvas.x && Math.abs(project.y - canvas.y) < 4,
    JSON.stringify({ project, editor, canvas }));
  check('default: Scene is a background tab (no box of its own)', (await box(page, '[data-panel=scene]'))?.w === 0 || (await box(page, '[data-panel=scene]')) === null);
  check('default: the panel area is collapsed: no Problems or Diagnostics box exists', (await box(page, '[data-panel=problems]')) === null && (await box(page, '[data-panel=diagnostics]')) === null);
  check('default: the status bar shows the Problems item with split counts', (await page.locator('#status-diagnostics-toggle').count()) === 1 &&
    (await page.locator('#status-diagnostics-toggle').getAttribute('aria-pressed')) === 'false');
  await snap(page, 'default-1440');

  await page.click('#status-diagnostics-toggle');
  await page.waitForTimeout(400);
  const bottom = await box(page, '[data-panel=problems]');
  check('the Problems item opens the panel area: Problems spans the width under the editor and canvas',
    Boolean(bottom) && bottom.y > editor.y + 100 && bottom.w > 800, JSON.stringify(bottom));
  check('opening the panel area did not touch the Canvas element', (await sameCanvas(page)) && (await removals(page)) === 0);
  await page.keyboard.press('Control+j');
  await page.waitForTimeout(300);
  check('Ctrl+J collapses it again (no Problems box)', (await box(page, '[data-panel=problems]')) === null);
  await page.keyboard.press('Control+j');
  await page.waitForTimeout(300);
  check('Ctrl+J opens it again', (await box(page, '[data-panel=problems]')) !== null);
  await page.keyboard.press('Control+j');
  await page.waitForTimeout(300);

  const before = { project: await box(page, '[data-panel=project]'), editor: await box(page, '[data-panel=editor]') };
  await page.click('#flexlayout-tabbutton-scene');
  await page.waitForTimeout(300);
  check('a tab switch inside a group (Canvas to Scene) moves no other panel',
    JSON.stringify(before) === JSON.stringify({ project: await box(page, '[data-panel=project]'), editor: await box(page, '[data-panel=editor]') }));
  check('the hidden Canvas has no box while Scene is the active tab', (await box(page, '[data-panel=canvas]'))?.w === 0);
  await page.click('#flexlayout-tabbutton-canvas');
  check('switching tabs never removed the Canvas element', (await sameCanvas(page)) && (await removals(page)) === 0);
  check('no page errors', errors.length === 0, errors.join(' | '));
  await context.close();
}

// ---- 2. open and close independently; closing Canvas takes no GPU device -------------------------------------

{
  const { page, context } = await open(1440, 900);
  const live = () => page.evaluate(() => (window.__tesseraGpu ? window.__tesseraGpu.live : 0));
  check('Canvas is open at boot', (await page.locator('#canvas').count()) === 1);
  await via(page, "wb.dispatch({kind:'collapse', groupId:'g-bottom', collapsed:false})");
  await via(page, "wb.commands.togglePanel('scene')");
  check('a hidden open panel is revealed by its toggle, not closed', groupOf(await model(page), 'scene') !== null && !(await model(page)).closed.includes('scene'));
  await via(page, "wb.commands.togglePanel('scene')");
  check('and a visible one is closed by it', (await model(page)).closed.includes('scene'));
  await via(page, "wb.commands.togglePanel('scene')");
  await page.waitForTimeout(300);
  await via(page, "wb.dispatch({kind:'close', panelId:'canvas'})");
  await page.waitForTimeout(300);
  check('closing Canvas removes the element and its pane', (await page.locator('#canvas').count()) === 0 && (await page.locator('[data-panel=canvas] *').count()) === 0);
  // `device.destroy()` resolves `device.lost` asynchronously; with a real device that takes longer than a tick.
  await page.waitForFunction(() => !window.__tesseraGpu || window.__tesseraGpu.live === 0, null, { timeout: 8000 }).catch(() => {});
  check('closing Canvas leaves no GPU device taken', (await live()) === 0, `live=${await live()}`);
  check('Scene and Diagnostics are unaffected by closing Canvas', (await page.locator('#scene-status').count()) === 1 && (await page.locator('#diag-status').count()) === 1);
  await via(page, "wb.dispatch({kind:'open', panelId:'canvas'})");
  await page.waitForTimeout(400);
  check('reopening Canvas renders a fresh element', (await page.locator('#canvas').count()) === 1 && !(await sameCanvas(page)));
  await via(page, "wb.dispatch({kind:'close', panelId:'scene'})");
  await via(page, "wb.dispatch({kind:'close', panelId:'diagnostics'})");
  await page.waitForTimeout(300);
  check('Scene and Diagnostics close independently', (await page.locator('#scene-status').count()) === 0 && (await page.locator('#diag-status').count()) === 0 && (await page.locator('#canvas').count()) === 1);
  await context.close();
}

// ---- 3. a real drag: Canvas to the bottom, Diagnostics to the right -----------------------------------------

{
  const { page, errors, context } = await open(1440, 900);
  await via(page, "wb.dispatch({kind:'collapse', groupId:'g-bottom', collapsed:false})");
  await page.waitForTimeout(400);
  const tab = await box(page, '#flexlayout-tabbutton-canvas');
  const editor = await box(page, '[data-panel=editor]');
  await drag(page, centre(tab), { x: editor.x + editor.w / 2, y: editor.y + editor.h - 30 });
  const m1 = await model(page);
  const canvas1 = await box(page, '[data-panel=canvas]');
  const editor1 = await box(page, '[data-panel=editor]');
  check('drag Canvas to the bottom of the editor: it is now stacked under the editor',
    groupOf(m1, 'canvas').id !== 'g-preview' && canvas1.y >= editor1.y + editor1.h - 4 && Math.abs(canvas1.x - editor1.x) < 6,
    JSON.stringify({ canvas1, editor1 }));
  check('  the same HTMLCanvasElement, 0 removals', (await sameCanvas(page)) && (await removals(page)) === 0, `removals=${await removals(page)}`);
  await snap(page, 'after-drag-canvas-bottom');

  const dtab = await box(page, '#flexlayout-tabbutton-diagnostics');
  const dock = await box(page, '.dock-host');
  await drag(page, centre(dtab), { x: dock.x + dock.w - 6, y: dock.y + dock.h / 2 });
  const m2 = await model(page);
  const diag = await box(page, '[data-panel=diagnostics]');
  const canvas2 = await box(page, '[data-panel=canvas]');
  check('drag Diagnostics to the right: it is a column at the right edge, beside everything else',
    Boolean(diag) && diag.x > canvas2.x + canvas2.w - 6 && m2.root.children.at(-1).type === 'group' && m2.root.children.at(-1).panels[0] === 'diagnostics',
    JSON.stringify({ diag, canvas2 }));
  check('  the Canvas element is still the same, 0 removals', (await sameCanvas(page)) && (await removals(page)) === 0);
  await snap(page, 'after-drag-diagnostics-right');
  await page.waitForTimeout(500);
  const stored = await page.evaluate(() => localStorage.getItem('tessera.studio.layout'));
  const keep = JSON.stringify(m2);
  await page.reload({ waitUntil: 'load' });
  await page.waitForSelector('.flexlayout__layout');
  await page.waitForTimeout(800);
  check('reload returns the same layout', JSON.stringify(await model(page)) === keep && stored !== null);
  check('no page errors during drags', errors.length === 0, errors.join(' | '));
  await context.close();
}

// ---- 4. keyboard-only docking --------------------------------------------------------------------------------

{
  const { page, context } = await open(1440, 900);
  await page.focus('#flexlayout-tabbutton-canvas');
  await page.keyboard.press('Alt+Shift+ArrowDown');
  await page.waitForTimeout(500);
  const m = await model(page);
  check('Alt+Shift+Down moves Canvas into the panel area and opens it', groupOf(m, 'canvas').id === 'g-bottom' && m.collapsed.length === 0);
  check('  focus returns to the moved panel\'s tab', (await page.evaluate(() => document.activeElement?.id)) === 'flexlayout-tabbutton-canvas');
  check('  an aria-live message states the result', (await page.locator('#workbench-live').textContent()) === 'Canvas moved to the bottom');
  check('  the Canvas element is the same, 0 removals', (await sameCanvas(page)) && (await removals(page)) === 0, `removals=${await removals(page)}`);
  await page.focus('#flexlayout-tabbutton-diagnostics');
  await page.keyboard.press('Alt+Shift+ArrowRight');
  await page.waitForTimeout(500);
  const m2 = await model(page);
  check('Alt+Shift+Right moves Diagnostics to a column at the right edge', m2.root.children.at(-1).panels?.[0] === 'diagnostics');
  check('  focus and the live message follow', (await page.evaluate(() => document.activeElement?.id)) === 'flexlayout-tabbutton-diagnostics' &&
    (await page.locator('#workbench-live').textContent()) === 'Diagnostics moved to the right');
  check('  the Canvas element is still the same, 0 removals', (await sameCanvas(page)) && (await removals(page)) === 0);
  await page.focus('#flexlayout-tabbutton-scene');
  await page.keyboard.press('Delete');
  await page.waitForTimeout(300);
  check('Delete closes the focused tab', (await model(page)).closed.includes('scene'));
  await context.close();
}

// ---- 5. storage that throws, a newer layout, no WebGPU ------------------------------------------------------

{
  const { page, errors, context } = await open(1440, 900, {
    init: () => {
      const refuse = () => { throw new Error('storage blocked'); };
      Storage.prototype.getItem = refuse;
      Storage.prototype.setItem = refuse;
    },
  });
  check('a throwing localStorage renders the default layout with no error',
    errors.length === 0 && (await box(page, '[data-panel=editor]')) !== null && (await model(page)).collapsed.join() === 'g-bottom', errors.join(' | '));
  await context.close();
}
{
  const newer = JSON.stringify({ format: 'tessera.studio.layout', version: 99, root: { type: 'group', id: 'g-x', panels: ['editor'], active: 'editor' }, maximized: null, closed: [], shell: {}, collapsed: [] });
  const { page, errors, context } = await open(1440, 900, { storage: { 'tessera.studio.layout': newer } });
  const backup = await page.evaluate(() => localStorage.getItem('tessera.studio.layout.backup'));
  check('a layout from a newer studio: default used, backup kept, no error', errors.length === 0 && backup === newer && (await model(page)).version === 2, errors.join(' | '));
  await context.close();
}
{
  const { page, context } = await open(1440, 900, { init: () => Object.defineProperty(navigator, 'gpu', { value: undefined, configurable: true }) });
  await via(page, "wb.dispatch({kind:'open', panelId:'scene'})");
  await via(page, "wb.dispatch({kind:'open', panelId:'diagnostics'})");
  await via(page, "wb.dispatch({kind:'activate', panelId:'canvas'})");
  await page.waitForTimeout(800);
  const unsupported = await page.evaluate(() => { const n = document.getElementById('unsupported'); return n !== null && !n.hidden; });
  check('no WebGPU: Canvas alone shows its unsupported state', unsupported);
  check('  Scene and Diagnostics panels still open and carry their controls', (await page.locator('#scene-status').count()) === 1 && (await page.locator('#diag-status').count()) === 1);
  await context.close();
}

// ---- 6. ARIA, splitter keys, maximize, reset ---------------------------------------------------------------

{
  const { page, context } = await open(1440, 900);
  const seps = await page.evaluate(() => [...document.querySelectorAll('[role=separator]')].map((e) => ({
    o: e.getAttribute('aria-orientation'), now: Number(e.getAttribute('aria-valuenow')), min: Number(e.getAttribute('aria-valuemin')), max: Number(e.getAttribute('aria-valuemax')), tab: e.tabIndex,
  })));
  check('separators: role, orientation, tabindex 0, and real min < now < max (not the library\'s 0..100)',
    seps.length >= 2 && seps.every((s) => s.o && s.tab === 0 && s.min < s.now && s.now < s.max && !(s.min === 0 && s.max === 100)), JSON.stringify(seps));
  const tablists = await page.evaluate(() => [...document.querySelectorAll('[role=tablist]')].filter((l) => l.querySelector('[id^=flexlayout-tabbutton-]')).map((l) => ({
    stops: [...l.querySelectorAll('[role=tab]')].filter((t) => t.tabIndex === 0).length, selected: [...l.querySelectorAll('[role=tab]')].filter((t) => t.getAttribute('aria-selected') === 'true').length,
  })));
  check('every tab strip has one selected tab and (at most) one tab stop', tablists.length >= 3 && tablists.every((l) => l.selected === 1 && l.stops <= 1), JSON.stringify(tablists));
  check('tabs name their panel (aria-controls) and each panel is a tabpanel with aria-labelledby',
    await page.evaluate(() => [...document.querySelectorAll('[id^=flexlayout-tabbutton-]')].every((t) => t.getAttribute('aria-controls')) && [...document.querySelectorAll('[id^=flexlayout-tab-]')].every((p) => p.getAttribute('aria-labelledby'))));

  const first = page.locator('[role=separator]').first();
  const value = async () => Number(await first.getAttribute('aria-valuenow'));
  const extent = await page.evaluate(() => document.querySelector('[role=separator]').parentElement.getBoundingClientRect().width);
  await first.focus();
  const v0 = await value();
  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(200);
  const v1 = await value();
  await page.keyboard.press('Shift+ArrowRight');
  await page.waitForTimeout(200);
  const v2 = await value();
  check('splitter: ArrowRight moves 16 px and Shift+ArrowRight 64 px',
    Math.abs((v1 - v0) - Math.round((16 / extent) * 100)) <= 1 && Math.abs((v2 - v1) - Math.round((64 / extent) * 100)) <= 1, `${v0} -> ${v1} -> ${v2} of ${Math.round(extent)} px`);
  await page.keyboard.press('Home');
  await page.waitForTimeout(200);
  const vmin = await value();
  await page.keyboard.press('End');
  await page.waitForTimeout(200);
  const vmax = await value();
  check('splitter: Home goes to aria-valuemin and End to aria-valuemax',
    vmin === Number(await first.getAttribute('aria-valuemin')) && vmax === Number(await first.getAttribute('aria-valuemax')) && vmin < vmax, `${vmin}..${vmax}`);

  // A real sash drag (the collapsed panel area makes this the folded tree: the real model must follow).
  await via(page, "wb.commands.resetLayout()");
  await page.waitForTimeout(400);
  const sash = await box(page, '[role=separator]');
  const sizeBefore = (await model(page)).root.sizes[0];
  await drag(page, centre(sash), { x: sash.x + sash.w / 2 + 140, y: centre(sash).y });
  const sizeAfter = (await model(page)).root.sizes[0];
  check('sash drag resizes: the real model follows the visible sash and the aria value moves with it',
    sizeAfter > sizeBefore + 0.05 && Number(await page.locator('[role=separator]').first().getAttribute('aria-valuenow')) === Math.round(sizeAfter * 100),
    `${sizeBefore} -> ${sizeAfter}`);
  check('  the collapsed panel area kept its share', JSON.stringify((await model(page)).root.children[1].sizes) === '[0.72,0.28]');
  await page.dblclick('#flexlayout-tabbutton-editor');
  await page.waitForTimeout(400);
  check('double-click on a tab strip maximizes its group; the others stay mounted but hidden',
    (await model(page)).maximized === 'editor' && (await box(page, '[data-panel=canvas]'))?.w === 0 && (await sameCanvas(page)) && (await removals(page)) === 0);
  await snap(page, 'maximized');
  await page.dblclick('#flexlayout-tabbutton-editor');
  await page.waitForTimeout(400);
  check('and double-click again restores', (await model(page)).maximized === null && (await box(page, '[data-panel=canvas]'))?.w > 100);

  const editorHost = await page.evaluateHandle(() => document.querySelector('[data-panel=editor]'));
  await via(page, "wb.dispatch({kind:'move', panelId:'canvas', target:{edge:'bottom', of:'g-editor'}})");
  await via(page, "wb.commands.resetLayout()");
  await page.waitForTimeout(500);
  const m = await model(page);
  check('Reset layout restores the default arrangement', JSON.stringify(m.root).includes('"g-preview"') && m.collapsed.join() === 'g-bottom');
  check('  and does not remount a panel that was unchanged (the editor host and its CodeMirror)',
    await page.evaluate((h) => document.querySelector('[data-panel=editor]') === h && h.querySelector('.cm-editor') !== null, editorHost));
  await context.close();
}

// ---- 7. the phone: one group, a switcher, no horizontal scroll, storage untouched ----------------------------

{
  const { page, errors, context } = await open(1440, 900);
  await via(page, "wb.dispatch({kind:'move', panelId:'canvas', target:{edge:'bottom', of:'g-editor'}})");
  await page.waitForTimeout(600);
  const desktop = await page.evaluate(() => localStorage.getItem('tessera.studio.layout'));
  await page.setViewportSize({ width: 390, height: 844 });
  await page.waitForTimeout(700);
  const strips = await page.evaluate(() => [...document.querySelectorAll('[role=tablist]')].filter((l) => l.querySelector('[id^=flexlayout-tabbutton-]')).length);
  const tabs = await page.evaluate(() => [...document.querySelectorAll('[id^=flexlayout-tabbutton-]')].map((t) => t.textContent.trim()));
  check('390 px: one visible group with a switcher (a single tab strip)', strips === 1, `strips=${strips}`);
  check('  every open panel is reachable from it, in phone order (Editor, Canvas, Scene, Diagnostics first)',
    tabs.slice(0, 4).join() === 'Editor,Canvas,Scene,Diagnostics' && tabs.includes('Project'), tabs.join());
  check('  no horizontal scroll', await page.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth));
  await page.click('#flexlayout-tabbutton-scene');
  await page.waitForTimeout(300);
  check('  a tab switches the visible panel', ((await box(page, '[data-panel=scene]'))?.w ?? 0) > 100 && ((await box(page, '[data-panel=editor]'))?.w ?? 0) === 0);
  await snap(page, 'phone-390');
  await page.waitForTimeout(500);
  check('  the persisted desktop layout did not change', (await page.evaluate(() => localStorage.getItem('tessera.studio.layout'))) === desktop);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.waitForTimeout(700);
  check('resizing back restores the desktop arrangement unchanged', JSON.stringify(await model(page)) === JSON.stringify(JSON.parse(desktop)));
  check('no page errors across the resize round trip', errors.length === 0, errors.join(' | '));
  await context.close();
}

await browser.close();
server?.close();
console.log(failures === 0 ? '\nall checks passed' : `\n${failures} check(s) FAILED`);
process.exit(failures === 0 ? 0 : 1);
