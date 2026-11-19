#!/usr/bin/env node
/**
 * Slice W0 proof for the studio workbench dock (docs/specs/studio-workbench-layout-v1.md sections 4 and 5):
 * flexlayout-react against the hard constraints of issue #516, in a real Chromium with a real WebGPU
 * canvas, using real pointer drags (mouse down / many moves / up, never a synthetic DragEvent) and
 * keyboard-only docking.
 *
 * Throwaway by design: it drives `designer/web/spike/` (deleted when W5 lands) and is superseded by the
 * rewritten `preview_browser_layout.mjs` (W6). Developer-run, not a CI gate: it needs a headed or
 * WebGPU-capable Chromium and Playwright (`playwright` is not a repository dependency).
 *
 * Usage:
 *   node scripts/ci/workbench_dock_spike.mjs [--url http://127.0.0.1:5391/spike/dock.html] [--shots dir]
 * Without --url it starts `vite` in designer/web on a free port. If `playwright` is not resolvable set
 * PLAYWRIGHT_DIR to a directory whose node_modules contains it.
 *
 * Each line is PASS, FAIL or GAP. GAP = the library does not provide the behavior; the adapter must
 * (recorded in the layout spec section 5.6). Exit code is 1 only for FAIL.
 */
import { spawn } from 'node:child_process';
import { mkdir } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const args = process.argv.slice(2);
const arg = (name, fallback) => (args.includes(name) ? args[args.indexOf(name) + 1] : fallback);
const shots = arg('--shots', null);
const here = dirname(fileURLToPath(import.meta.url));

async function loadPlaywright() {
  try {
    return await import('playwright');
  } catch {
    const dir = process.env.PLAYWRIGHT_DIR;
    if (!dir) throw new Error('playwright not found: npm install playwright, or set PLAYWRIGHT_DIR');
    const require = createRequire(join(resolve(dir), 'noop.js'));
    return await import(pathToFileURL(require.resolve('playwright')).href);
  }
}

async function startVite() {
  const port = 5400 + Math.floor(Math.random() * 400);
  const cwd = resolve(here, '..', '..', 'designer', 'web');
  const child = spawn('npx', ['vite', '--port', String(port), '--strictPort', '--host', '127.0.0.1'], {
    cwd,
    shell: true,
    stdio: 'ignore',
  });
  const url = `http://127.0.0.1:${port}/spike/dock.html`;
  for (let i = 0; i < 60; i += 1) {
    try {
      if ((await fetch(url)).ok) return { url, stop: () => child.kill() };
    } catch {
      // not up yet
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  child.kill();
  throw new Error('vite did not start');
}

const results = [];
function record(status, name, detail = '') {
  results.push(status);
  console.log(`${status.padEnd(4)} ${name}${detail ? ` -- ${detail}` : ''}`);
}
const check = (ok, name, detail) => record(ok ? 'PASS' : 'FAIL', name, detail);

const pw = await loadPlaywright();
const chromium = pw.chromium ?? pw.default.chromium;
const vite = arg('--url', null) ? null : await startVite();
const base = arg('--url', vite?.url);
if (shots) await mkdir(shots, { recursive: true });

const browser = await chromium.launch({ headless: false, args: ['--enable-unsafe-webgpu'] });
const errors = [];

async function open(query = '', viewport = { width: 1440, height: 900 }) {
  const page = await browser.newPage({ viewport });
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => {
    if (m.type() === 'error' && !/404|panel render failure|<Boom>/.test(m.text())) errors.push(m.text());
  });
  await page.goto(base + query);
  await page.waitForFunction(() => window.__spike && document.querySelector('[role=tablist]'));
  await page.waitForTimeout(600);
  return page;
}

const center = (r) => ({ x: r.x + r.width / 2, y: r.y + r.height / 2 });
const rectOf = (page, selector) => page.locator(selector).first().boundingBox();
const snap = (page, name) => (shots ? page.screenshot({ path: join(shots, `${name}.png`) }) : null);

/** A real pointer drag: press, many small moves (so the drop target tracks), release. */
async function drag(page, from, to) {
  await page.mouse.move(from.x, from.y);
  await page.mouse.down();
  await page.mouse.move(from.x + 12, from.y + 12, { steps: 6 });
  await page.mouse.move(to.x, to.y, { steps: 25 });
  await page.waitForTimeout(250);
  await page.mouse.move(to.x + 1, to.y + 1, { steps: 3 });
  await page.waitForTimeout(150);
  await page.mouse.up();
  await page.waitForTimeout(500);
}

const parentOf = (page, id) => page.evaluate((i) => window.__spike.model.getNodeById(i)?.getParent()?.getId(), id);
const state = (page) =>
  page.evaluate(() => {
    const s = window.__spike;
    const el = document.getElementById('spike-canvas');
    return {
      removals: s.probe.canvasRemovals,
      mounts: s.probe.canvasMounts,
      sameNode: s.probe.canvasNodes.length === 1 && el === s.probe.canvasNodes[0],
      contexts: s.stats.contextCreations,
      frames: s.stats.frames,
      gpu: s.stats.gpu,
    };
  });

// ---------------------------------------------------------------------------------------------- default
const page = await open();
let st = await state(page);
check(st.gpu, 'real WebGPU adapter and context acquired', `frames=${st.frames}`);
check(st.mounts === 1 && st.contexts === 1 && st.removals === 0, 'canvas mounted once, one GPU context', JSON.stringify(st));
const order = await page.evaluate(() => ['project', 'editor', 'canvas'].map((id) => document.getElementById(`flexlayout-tab-${id}`).getBoundingClientRect().x));
check(order[0] < order[1] && order[1] < order[2], 'Project | Editor | Canvas side by side', order.map(Math.round).join(' < '));
await snap(page, 'dock-1-default');

await page.fill('#editor-input', 'typed before moves');
await page.evaluate(() => { document.getElementById('editor-input').__marker = 1; });

// ------------------------------------------------------------------------- real pointer drag: tab join
const canvasTab = center(await rectOf(page, '#flexlayout-tabbutton-canvas'));
const boomTab = center(await rectOf(page, '#flexlayout-tabbutton-boom'));
await drag(page, canvasTab, boomTab);
await snap(page, 'dock-2-after-drag-to-bottom-tab');
check((await parentOf(page, 'canvas')) === 'g-bottom', 'pointer drag: Canvas tab joins the bottom group', `parent=${await parentOf(page, 'canvas')}`);
st = await state(page);
check(st.sameNode && st.removals === 0 && st.mounts === 1 && st.contexts === 1, 'canvas same object, 0 removals, 1 mount, 1 context after the drag', JSON.stringify(st));
const f0 = st.frames;
await page.waitForTimeout(500);
const f1 = (await state(page)).frames;
check(f1 > f0, 'canvas keeps drawing after the move (visible in its new group)', `${f0} -> ${f1}`);

// ------------------------------------------------------------------- real pointer drag: edge split
const canvasTab2 = center(await rectOf(page, '#flexlayout-tabbutton-canvas'));
const editorBody = await rectOf(page, '#flexlayout-tab-editor');
await drag(page, canvasTab2, { x: editorBody.x + editorBody.width - 6, y: editorBody.y + editorBody.height / 2 });
await snap(page, 'dock-3-after-drag-edge-split');
const canvasParent = await parentOf(page, 'canvas');
check(canvasParent !== 'g-bottom' && canvasParent !== 'g-preview', 'pointer drag: Canvas dropped on the editor right edge creates a new split', `parent=${canvasParent}`);
st = await state(page);
check(st.sameNode && st.removals === 0 && st.mounts === 1 && st.contexts === 1, 'canvas same object, 0 removals after the edge split', JSON.stringify(st));
check(await page.evaluate(() => document.getElementById('editor-input').__marker === 1 && document.getElementById('editor-input').value === 'typed before moves'), 'editor DOM node and typed text survive the moves');

// ------------------------------------------------------------------------------ hidden never draws
await page.evaluate(() => window.__spike.model.doAction(window.__spike.Actions.moveNode('canvas', 'g-preview', window.__spike.DockLocation.CENTER, -1)));
await page.waitForTimeout(300);
await page.click('#flexlayout-tabbutton-scene');
await page.waitForTimeout(300);
const h0 = (await state(page)).frames;
await page.waitForTimeout(1000);
const h1 = (await state(page)).frames;
check(h1 === h0, 'hidden Canvas (Scene tab active) produces zero frames over 1 s', `${h0} -> ${h1}`);
await page.click('#flexlayout-tabbutton-canvas');
await page.waitForTimeout(500);
check((await state(page)).frames > h1, 'showing it again resumes drawing');
st = await state(page);
check(st.sameNode && st.removals === 0, 'hide/show leaves the same canvas object, 0 removals', JSON.stringify(st));

// ------------------------------------------------------------------------------------ keyboard-only docking
await page.focus('#flexlayout-tabbutton-canvas');
await page.keyboard.press('Alt+Shift+ArrowDown');
await page.waitForTimeout(500);
check((await parentOf(page, 'canvas')) === 'g-bottom', 'keyboard-only: Alt+Shift+Down docks Canvas to the bottom group', `parent=${await parentOf(page, 'canvas')}`);
check((await page.evaluate(() => document.activeElement?.id)) === 'flexlayout-tabbutton-canvas', 'focus returns to the moved panel tab');
check(/canvas moved to the bottom/.test(await page.textContent('#spike-live')), 'aria-live message states the result');
await page.keyboard.press('Alt+Shift+ArrowRight');
await page.waitForTimeout(500);
check(!['g-bottom', 'g-preview'].includes(await parentOf(page, 'canvas')), 'keyboard-only: Alt+Shift+Right splits Canvas into a new group on the right', `parent=${await parentOf(page, 'canvas')}`);
st = await state(page);
check(st.sameNode && st.removals === 0 && st.contexts === 1, 'canvas same object after keyboard moves', JSON.stringify(st));

// ------------------------------------------------------------------------------------------ ARIA 4.4
const aria = await page.evaluate(() => {
  const q = (s) => Array.from(document.querySelectorAll(s));
  const lists = q('[role=tablist]');
  return {
    tablists: lists.length,
    orient: lists.every((l) => l.getAttribute('aria-orientation')),
    tabs: q('[role=tab]').every((t) => t.hasAttribute('aria-selected') && t.hasAttribute('aria-controls')),
    panels: q('[role=tabpanel]').every((t) => t.hasAttribute('aria-labelledby')),
    oneStop: lists.every((l) => l.querySelectorAll('[role=tab][tabindex="0"]').length === 1),
    seps: q('[role=separator]').map((s) => ({ o: s.getAttribute('aria-orientation'), n: s.getAttribute('aria-valuenow'), min: s.getAttribute('aria-valuemin'), max: s.getAttribute('aria-valuemax'), t: s.getAttribute('tabindex') })),
  };
});
check(aria.tablists >= 3 && aria.orient, 'ARIA: every tab strip is a tablist with aria-orientation', `${aria.tablists} tablists`);
check(aria.tabs, 'ARIA: tabs have aria-selected and aria-controls');
check(aria.panels, 'ARIA: tabpanels have aria-labelledby');
check(aria.oneStop, 'ARIA: one tab stop (tabindex 0) per tablist');
check(aria.seps.length >= 3 && aria.seps.every((s) => s.o && s.n !== null && s.t === '0'), 'ARIA: splitters are separators with orientation, valuenow and tabindex 0', JSON.stringify(aria.seps[0]));
record(aria.seps.every((s) => s.min === '0' && s.max === '100') ? 'GAP' : 'PASS', 'ARIA: separator min/max are the constant 0..100, not the per-splitter range of spec 4.4', 'adapter may override aria-valuemin/max');

// Arrow keys on a splitter (fresh page so the geometry is the default one), and the tab arrow keys.
const page6 = await open();
const sepX = () => page6.evaluate(() => Math.round(document.querySelector('[role=separator]').getBoundingClientRect().x));
await page6.locator('[role=separator]').first().focus();
const x0 = await sepX();
await page6.keyboard.press('ArrowRight');
await page6.waitForTimeout(450);
const x1 = await sepX();
check(x1 > x0, 'splitter resizes with the arrow keys', `${x0} -> ${x1} px (library step ${x1 - x0} px)`);
await page6.keyboard.press('Shift+ArrowRight');
await page6.waitForTimeout(450);
const x2 = await sepX();
record(x2 - x1 >= 60 ? 'PASS' : 'GAP', 'Shift+arrow resizes by 64 px', `step ${x2 - x1} px; spec 4.4 wants 16 px / Shift 64 px / Home / End, the adapter adds them`);
await page6.keyboard.press('Home');
await page6.waitForTimeout(450);
record((await sepX()) < x1 - 20 ? 'PASS' : 'GAP', 'Home jumps the splitter to its minimum', `x=${await sepX()}`);
await page6.focus('#flexlayout-tabbutton-scene');
await page6.keyboard.press('ArrowLeft');
const afterArrow = await page6.evaluate(() => document.activeElement?.id);
check(afterArrow === 'flexlayout-tabbutton-canvas', 'Left/Right move focus between the tabs of a strip', `focus -> ${afterArrow}`);
await page6.close();
await page.focus('#flexlayout-tabbutton-boom');
await page.keyboard.press('Alt+w');
await page.waitForTimeout(300);
check((await page.evaluate(() => window.__spike.model.getNodeById('boom'))) == null, 'Alt+W closes the focused tab (keyMap closeTab)');
await snap(page, 'dock-4-after-keyboard');

// ------------------------------------------------------------------------------------ failure isolation
const page2 = await open();
await page2.click('#flexlayout-tabbutton-boom');
await page2.click('#boom-button');
await page2.waitForTimeout(300);
check((await page2.locator('[data-panel-failed="boom"]').count()) === 1, 'a throwing panel shows "This panel failed" in that panel only');
const st2 = await state(page2);
check(st2.sameNode && st2.removals === 0 && (await page2.locator('#flexlayout-tabbutton-diagnostics').count()) === 1, 'siblings (Canvas, Diagnostics) untouched by the failure', JSON.stringify(st2));
await snap(page2, 'dock-5-failure-isolation');
await page2.close();

// ------------------------------------------------------------------- lazy import, Canvas in a background tab
const page3 = await open('?bg=1');
const lazy = await page3.evaluate(() => ({ imported: window.__spike.stats.importedAt, frames: window.__spike.stats.frames, mounts: window.__spike.probe.canvasMounts }));
check(lazy.imported === null && lazy.frames === 0, 'Canvas restored hidden imports no module and draws nothing', JSON.stringify(lazy));
check(!(await page3.evaluate(() => performance.getEntriesByType('resource').some((r) => r.name.includes('canvas-module')))), 'the lazy module is not even fetched while the panel is hidden');
await page3.click('#flexlayout-tabbutton-canvas');
await page3.waitForTimeout(800);
const lazy2 = await state(page3);
check(lazy2.frames > 0 && lazy2.contexts === 1, 'first visibility imports the module and starts drawing', JSON.stringify(lazy2));
await page3.close();

// ------------------------------------------------------------ collapsed bottom group (border experiment)
const page4 = await open('?bottom=border');
const borderTabs = await page4.locator('#flexlayout-tabbutton-diagnostics').count();
const diagVisible = await page4.evaluate(() => { const e = document.getElementById('flexlayout-tab-diagnostics'); return !!e && e.getBoundingClientRect().height > 0; });
record(borderTabs === 1 && !diagVisible ? 'PASS' : 'GAP', 'bottom border starts collapsed (strip only, content hidden)', `strip=${borderTabs} contentVisible=${diagVisible}`);
await page4.click('#flexlayout-tabbutton-diagnostics');
await page4.waitForTimeout(300);
const opened = await page4.evaluate(() => document.getElementById('flexlayout-tab-diagnostics')?.getBoundingClientRect().height > 0);
check(opened, 'border opens on click');
st = await state(page4);
check(st.sameNode && st.removals === 0 && st.contexts === 1, 'toggling the border leaves the Canvas object untouched', JSON.stringify(st));
await snap(page4, 'dock-6-border-open');
await page4.close();

// ------------------------------------------------------------------------------------- phone width + JSON
const page5 = await open('', { width: 390, height: 844 });
const noScroll = await page5.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth);
check(noScroll, '390 px: no horizontal scroll (library only; the phone collapse is ours, spec 3.4)');
await snap(page5, 'dock-7-phone');
const roundTrip = await page5.evaluate(() => {
  const m = window.__spike.model;
  const again = m.constructor.fromJson(m.toJson());
  return JSON.stringify(again.toJson()) === JSON.stringify(m.toJson());
});
check(roundTrip, 'library JSON round trip is stable (model to JSON to model to JSON)');
await page5.close();

check(errors.length === 0, 'no console errors or page errors', errors.slice(0, 3).join(' | '));
await page.close();
await browser.close();
vite?.stop();
const failed = results.filter((r) => r === 'FAIL').length;
console.log(`\n${results.filter((r) => r === 'PASS').length} pass, ${results.filter((r) => r === 'GAP').length} gap, ${failed} fail`);
process.exit(failed ? 1 : 0);
