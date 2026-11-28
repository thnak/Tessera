#!/usr/bin/env node
/**
 * Verify the studio's **dock layout** in a real browser (issue #563, slice W6 of #516;
 * `docs/specs/studio-workbench-layout-v1.md` section 4.5).
 *
 * The page is a dock workbench: Project, Editor and Canvas are panels the viewer arranges, not three fixed columns.
 * Before this rewrite the script asserted three side-by-side columns and that a view switch moved nothing; those
 * sentences are restated here per dock group. Boxes are read from `data-panel` attributes.
 *
 * Developer-run and not a CI gate: it needs Chromium (and a WebGPU-capable one for the frame assertions, which
 * print `skip` and never pass vacuously when the engine reports no GPU). The pure parts (model operations,
 * migrations, reconciliation, when-clauses, keybinding resolution) are Node gates.
 *
 * Usage:
 *   node scripts/ci/preview_browser_layout.mjs [--site designer/web/dist] [--shots dir] [--only 1,4,7]
 * Playwright: install it anywhere and set PLAYWRIGHT_DIR (see `studio_browser.mjs`).
 *
 * The nine assertions, at 1440x900 unless said (each prints `[n]` in its name so a mutation run can target it):
 *   1  default order Project | Editor | Canvas, panel area collapsed until Ctrl+J, status bar below with the
 *      Diagnostics item;
 *   2  a tab switch inside a group moves no other panel;
 *   3  Canvas, Scene and Diagnostics open and close independently from View > Panels; closing Canvas takes no
 *      GPU device;
 *   4  Canvas to the bottom and Diagnostics to the right by keyboard commands and by drag: the Canvas element is
 *      the same object, 0 removals, no re-init, frames keep increasing;
 *   5  reload returns the same layout; a throwing localStorage renders the default with no console error;
 *   6  a hidden Canvas produces zero frames over one second;
 *   7  at 390x844: one visible group, a switcher, no horizontal scroll, every panel reachable, the desktop layout
 *      in storage unchanged after a resize round trip;
 *   8  ARIA: roles, one tab stop per roving set, separator values;
 *   9  Reset Layout restores the default and does not change the open file or its dirty state.
 */

import { mkdir } from 'node:fs/promises';
import { join } from 'node:path';

import { argument, boxOf, loadChromium, openPage, openProject, reporter, serve } from './studio_browser.mjs';

const site = argument('--site', 'designer/web/dist');
const shots = argument('--shots', null);
const only = argument('--only', null)?.split(',').map(Number) ?? null;
const wants = (n) => only === null || only.includes(n);

const chromium = await loadChromium();
const { base, close } = await serve(site);
console.log(`testing ${base}`);
const browser = await chromium.launch({
  executablePath: process.env.PLAYWRIGHT_CHROMIUM || undefined,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader', '--disable-vulkan-surface'],
});
const { state, check, skip } = reporter();

const snap = async (page, name) => {
  if (shots) {
    await mkdir(shots, { recursive: true });
    await page.screenshot({ path: join(shots, `${name}.png`) });
  }
};
const model = (page) => page.evaluate(() => window.__tesseraWorkbench.layout());
const via = (page, source) => page.evaluate((s) => new Function('wb', `return (${s})`)(window.__tesseraWorkbench), source);
const hidden = async (page, selector) => {
  const box = await boxOf(page, selector);
  return box === null || box.w === 0 || box.h === 0;
};
const groupOf = (layout, id) => {
  const walk = (node) => (node.type === 'group' ? (node.panels.includes(id) ? node : null) : node.children.map(walk).find(Boolean) ?? null);
  return walk(layout.root);
};
const centre = (box) => ({ x: box.x + box.w / 2, y: box.y + box.h / 2 });
const gpu = (page) => page.evaluate(() => ({ acquired: window.__tesseraGpu?.acquired ?? 0, live: window.__tesseraGpu?.live ?? 0 }));
const frames = (page) => page.evaluate(() => Number(document.getElementById('stat-frame')?.textContent?.replace(/[^0-9]/g, '') || 0));

/** Records removals of the Canvas element and keeps a reference to it, so "the same object" is checkable. */
const probe = (page) =>
  page.evaluate(() => {
    window.__probe = { removals: 0, canvas: document.getElementById('canvas') };
    new MutationObserver((records) => {
      for (const record of records) for (const node of record.removedNodes) {
        if (window.__probe.canvas && (node === window.__probe.canvas || (node.contains && node.contains(window.__probe.canvas)))) window.__probe.removals += 1;
      }
    }).observe(document.documentElement, { childList: true, subtree: true });
  });
const sameCanvas = (page) => page.evaluate(() => window.__probe.canvas !== null && document.getElementById('canvas') === window.__probe.canvas);
const removals = (page) => page.evaluate(() => window.__probe.removals);

async function drag(page, from, to) {
  await page.mouse.move(from.x, from.y);
  await page.mouse.down();
  for (let step = 1; step <= 30; step += 1) {
    await page.mouse.move(from.x + ((to.x - from.x) * step) / 30, from.y + ((to.y - from.y) * step) / 30);
  }
  await page.waitForTimeout(450);
  await page.mouse.up();
  await page.waitForTimeout(450);
}

/** View > Panels > <title>, through the real menu (the keyboard and pointer path a person uses). */
async function togglePanelFromMenu(page, title) {
  await page.getByRole('menuitem', { name: 'View', exact: true }).first().click();
  await page.locator('[role=menu] [role^=menuitem]', { hasText: /^Panels/ }).first().click();
  await page.locator('[role=menu] [role^=menuitem]', { hasText: new RegExp(`^\u2713?${title}$`) }).first().click();
  await page.waitForTimeout(400);
}

// ---- [1] default layout, [2] tab switch ------------------------------------------------------------------------

if (wants(1) || wants(2)) {
  const { page, errors, context } = await openPage(browser, base, 1440, 900);
  await probe(page);
  const project = await boxOf(page, '[data-panel=project]');
  const editor = await boxOf(page, '[data-panel=editor]');
  const canvas = await boxOf(page, '[data-panel=canvas]');
  const footer = await boxOf(page, '.page-footer');
  check('[1] default: Project | Editor | Canvas side by side, in that order',
    Boolean(project && editor && canvas) && project.x < editor.x && editor.x < canvas.x && Math.abs(project.y - editor.y) < 4 && Math.abs(editor.y - canvas.y) < 4,
    JSON.stringify({ project, editor, canvas }));
  check('[1] default: the panel area is collapsed (no Diagnostics or Problems box)', (await hidden(page, '[data-panel=diagnostics]')) && (await hidden(page, '[data-panel=problems]')));
  check('[1] default: the status bar is below the dock and carries the Diagnostics item',
    Boolean(footer) && footer.y >= editor.y + editor.h - 2 && (await page.locator('#status-diagnostics-toggle').count()) === 1 &&
      (await page.locator('#status-diagnostics-toggle').getAttribute('aria-pressed')) === 'false', JSON.stringify(footer));
  await snap(page, 'layout-default-1440');
  await page.keyboard.press('Control+j');
  await page.waitForTimeout(400);
  const bottom = await boxOf(page, '[data-panel=problems]');
  check('[1] Ctrl+J opens the panel area: a bottom group under Editor and Canvas spanning their width, above the status bar',
    Boolean(bottom) && bottom.y >= editor.y + 100 && Math.abs(bottom.x - editor.x) <= 4 && Math.abs(bottom.x + bottom.w - (canvas.x + canvas.w)) <= 4 && bottom.y + bottom.h <= footer.y + 2, JSON.stringify(bottom));
  await page.keyboard.press('Control+j');
  await page.waitForTimeout(300);

  const before = { project: await boxOf(page, '[data-panel=project]'), editor: await boxOf(page, '[data-panel=editor]'), footer: await boxOf(page, '.page-footer') };
  await page.click('#flexlayout-tabbutton-scene');
  await page.waitForTimeout(300);
  const during = { project: await boxOf(page, '[data-panel=project]'), editor: await boxOf(page, '[data-panel=editor]'), footer: await boxOf(page, '.page-footer') };
  check('[2] Canvas to Scene (a tab switch inside the group) changes no other panel\'s box', JSON.stringify(before) === JSON.stringify(during));
  check('[2] the hidden Canvas has no box while Scene is the active tab', await hidden(page, '[data-panel=canvas]'));
  await page.click('#flexlayout-tabbutton-canvas');
  await page.waitForTimeout(300);
  check('[2] and back: every box is as it was, and the Canvas element was never removed',
    JSON.stringify(before) === JSON.stringify({ project: await boxOf(page, '[data-panel=project]'), editor: await boxOf(page, '[data-panel=editor]'), footer: await boxOf(page, '.page-footer') }) &&
      (await sameCanvas(page)) && (await removals(page)) === 0);
  check('no console errors at 1440x900', errors.length === 0, errors.slice(0, 3).join(' | ') || 'none');
  await context.close();
}

// ---- [3] open and close independently from View > Panels ---------------------------------------------------------

if (wants(3)) {
  const { page, context } = await openPage(browser, base, 1440, 900);
  const closed = async () => (await model(page)).closed.sort().join();
  const active = async (id) => groupOf(await model(page), id)?.active;
  // A background tab's toggle reveals it (the menu shows it unchecked); a visible panel's toggle closes it.
  await togglePanelFromMenu(page, 'Scene');
  check('[3] View > Panels > Scene on a background tab reveals it and closes nothing', (await active('scene')) === 'scene' && (await closed()) === '' && (await page.locator('#canvas').count()) === 1);
  await togglePanelFromMenu(page, 'Scene');
  check('[3] and again closes Scene only: Canvas and the editor are untouched', (await closed()) === 'scene' && (await page.locator('#scene-status').count()) === 0 && (await page.locator('#canvas').count()) === 1);
  await togglePanelFromMenu(page, 'Diagnostics');
  await togglePanelFromMenu(page, 'Diagnostics');
  check('[3] Diagnostics reveals then closes independently of Scene', (await closed()) === 'diagnostics,scene', await closed());
  await togglePanelFromMenu(page, 'Canvas');
  await page.waitForFunction(() => !window.__tesseraGpu || window.__tesseraGpu.live === 0, null, { timeout: 8000 }).catch(() => {});
  const taken = await gpu(page);
  check('[3] closing Canvas removes its element and leaves no GPU device taken', (await closed()) === 'canvas,diagnostics,scene' && (await page.locator('#canvas').count()) === 0 && taken.live === 0, JSON.stringify(taken));
  check('[3] all three are closed and the editor is untouched', (await boxOf(page, '[data-panel=editor]')) !== null && (await page.locator('#editor-host').count()) === 1);
  for (const title of ['Canvas', 'Scene', 'Diagnostics']) await togglePanelFromMenu(page, title);
  check('[3] each reopens from the same menu', (await closed()) === '' && (await page.locator('#canvas').count()) === 1, await closed());
  await context.close();
}

// ---- [4] moves by keyboard and by drag, [6] hidden Canvas draws nothing -------------------------------------------

if (wants(4) || wants(6)) {
  const { page, errors, context } = await openPage(browser, base, 1440, 900);
  await probe(page);
  const startGpu = await gpu(page);
  const hasGpu = startGpu.acquired > 0;
  const frame0 = await frames(page);
  await page.focus('#flexlayout-tabbutton-canvas');
  await page.keyboard.press('Alt+Shift+ArrowDown');
  await page.waitForTimeout(600);
  const m1 = await model(page);
  check('[4] keyboard: Alt+Shift+Down moves Canvas into the panel area (bottom) and opens it', groupOf(m1, 'canvas')?.id === 'g-bottom' && m1.collapsed.length === 0);
  await page.focus('#flexlayout-tabbutton-diagnostics');
  await page.keyboard.press('Alt+Shift+ArrowRight');
  await page.waitForTimeout(600);
  const m2 = await model(page);
  check('[4] keyboard: Alt+Shift+Right moves Diagnostics to a column at the right edge', m2.root.children?.at(-1)?.panels?.[0] === 'diagnostics', JSON.stringify(m2.root.children?.at(-1)));
  check('[4] keyboard moves: the Canvas element is the same object, 0 removals', (await sameCanvas(page)) && (await removals(page)) === 0, `removals=${await removals(page)}`);
  await snap(page, 'layout-moved-by-keyboard');

  await via(page, 'wb.commands.resetLayout()');
  await page.waitForTimeout(500);
  await via(page, "wb.dispatch({kind:'collapse', groupId:'g-bottom', collapsed:false})");
  await page.waitForTimeout(400);
  const tab = await boxOf(page, '#flexlayout-tabbutton-canvas');
  const editor = await boxOf(page, '[data-panel=editor]');
  await drag(page, centre(tab), { x: editor.x + editor.w / 2, y: editor.y + editor.h - 30 });
  const d1 = await model(page);
  const canvasAfter = await boxOf(page, '[data-panel=canvas]');
  const editorAfter = await boxOf(page, '[data-panel=editor]');
  check('[4] drag: Canvas dropped on the bottom of the editor is stacked under it',
    groupOf(d1, 'canvas')?.id !== 'g-preview' && canvasAfter.y >= editorAfter.y + editorAfter.h - 4, JSON.stringify({ canvasAfter, editorAfter }));
  const dtab = await boxOf(page, '#flexlayout-tabbutton-diagnostics');
  const dock = await boxOf(page, '.dock-host');
  await drag(page, centre(dtab), { x: dock.x + dock.w - 6, y: dock.y + dock.h / 2 });
  const d2 = await model(page);
  check('[4] drag: Diagnostics dropped on the right edge is a column there', d2.root.children?.at(-1)?.panels?.[0] === 'diagnostics', JSON.stringify(d2.root.children?.at(-1)));
  check('[4] drags: the Canvas element is the same object, 0 removals', (await sameCanvas(page)) && (await removals(page)) === 0, `removals=${await removals(page)}`);
  await snap(page, 'layout-moved-by-drag');
  if (hasGpu) {
    const reinit = (await gpu(page)).acquired;
    const a = await frames(page);
    await page.waitForTimeout(1000);
    const b = await frames(page);
    check('[4] frames keep increasing after the moves, with no GPU re-init', b > a && reinit === startGpu.acquired, `frames ${frame0} -> ${a} -> ${b}, acquired ${startGpu.acquired} -> ${reinit}`);
  } else {
    skip('[4] frames keep increasing and no re-init', 'no WebGPU adapter in this Chromium; the identity and removal counts above still ran');
  }
  check('[4] no console errors across the moves', errors.length === 0, errors.slice(0, 3).join(' | ') || 'none');

  if (wants(6)) {
    await via(page, 'wb.commands.resetLayout()');
    await page.waitForTimeout(500);
    await page.click('#flexlayout-tabbutton-scene');
    await page.waitForTimeout(500);
    check('[6] the Canvas is hidden behind Scene', await hidden(page, '[data-panel=canvas]'));
    if (hasGpu) {
      const f0 = await frames(page);
      await page.waitForTimeout(1000);
      const f1 = await frames(page);
      check('[6] a hidden Canvas produces zero frames over 1 s', f1 === f0, `frames ${f0} -> ${f1}`);
      await page.click('#flexlayout-tabbutton-canvas');
      await page.waitForTimeout(1000);
      check('[6] and it draws again when shown', (await frames(page)) > f1);
    } else {
      skip('[6] a hidden Canvas produces zero frames over 1 s', 'no WebGPU adapter in this Chromium (the counter is flat even when visible, so the assertion would pass vacuously)');
    }
  }
  await context.close();
}

// ---- [5] reload, throwing storage ---------------------------------------------------------------------------------

if (wants(5)) {
  const { page, context } = await openPage(browser, base, 1440, 900);
  await via(page, "wb.dispatch({kind:'collapse', groupId:'g-bottom', collapsed:false})");
  await via(page, "wb.commands.movePanelDirection('canvas', 'bottom')");
  await page.waitForTimeout(600);
  const kept = JSON.stringify(await model(page));
  await page.reload({ waitUntil: 'load' });
  await page.waitForSelector('.flexlayout__layout');
  await page.waitForTimeout(800);
  check('[5] reload returns the same layout (serialized models are equal)', JSON.stringify(await model(page)) === kept && JSON.parse(kept).collapsed.length === 0 && groupOf(JSON.parse(kept), 'canvas')?.id === 'g-bottom');
  await context.close();
}
if (wants(5)) {
  const { page, errors, context } = await openPage(browser, base, 1440, 900, {
    init: () => {
      const refuse = () => { throw new Error('storage blocked'); };
      Storage.prototype.getItem = refuse;
      Storage.prototype.setItem = refuse;
      Storage.prototype.removeItem = refuse;
    },
  });
  const m = await model(page);
  check('[5] a throwing localStorage renders the default layout with no console error',
    errors.length === 0 && m.collapsed.join() === 'g-bottom' && (await boxOf(page, '[data-panel=editor]')) !== null, errors.join(' | '));
  await context.close();
}

// ---- [7] phone, [8] ARIA, [9] Reset Layout --------------------------------------------------------------------------

if (wants(7)) {
  const { page, errors, context } = await openPage(browser, base, 390, 844);
  await page.waitForTimeout(400);
  const stored = await page.evaluate(() => localStorage.getItem('tessera.studio.layout'));
  const metrics = await page.evaluate(() => ({ scrollWidth: document.documentElement.scrollWidth, clientWidth: document.documentElement.clientWidth, body: document.body.scrollWidth }));
  const strips = await page.evaluate(() => [...document.querySelectorAll('[role=tablist]')].filter((l) => l.querySelector('[id^=flexlayout-tabbutton-]') && l.getBoundingClientRect().width > 0).length);
  check('[7] 390 px: one visible group with a switcher (one tab strip)', strips === 1, `strips=${strips}`);
  check('[7] 390 px: no horizontal scroll', metrics.scrollWidth <= metrics.clientWidth && metrics.body <= metrics.clientWidth, JSON.stringify(metrics));
  const titles = await page.evaluate(() => [...document.querySelectorAll('[id^=flexlayout-tabbutton-]')].map((t) => t.id.replace('flexlayout-tabbutton-', '')));
  const expected = ['editor', 'canvas', 'scene', 'diagnostics', 'project'];
  check('[7] 390 px: every open panel is reachable from the switcher', expected.every((id) => titles.includes(id)), titles.join());
  await page.click('#flexlayout-tabbutton-scene');
  await page.waitForTimeout(300);
  check('[7] 390 px: a tab shows its panel', !(await hidden(page, '[data-panel=scene]')) && (await hidden(page, '[data-panel=editor]')));
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.waitForTimeout(600);
  const project = await boxOf(page, '[data-panel=project]');
  const editor = await boxOf(page, '[data-panel=editor]');
  const canvas = await boxOf(page, '[data-panel=canvas]');
  check('[7] back at 1440 px the desktop arrangement is restored', Boolean(project && editor && canvas) && project.x < editor.x && editor.x < canvas.x);
  const after = await page.evaluate(() => localStorage.getItem('tessera.studio.layout'));
  check('[7] the persisted desktop layout is unchanged by the resize round trip', stored === after || (stored === null && after === null) || JSON.stringify(JSON.parse(after ?? 'null')?.root) === JSON.stringify(JSON.parse(stored ?? 'null')?.root));
  await snap(page, 'layout-phone-roundtrip');
  check('no console errors at 390x844', errors.length === 0, errors.slice(0, 3).join(' | ') || 'none');
  await context.close();
}

if (wants(8)) {
  const { page, context } = await openPage(browser, base, 1440, 900);
  const separators = await page.evaluate(() => [...document.querySelectorAll('.dock-host .flexlayout__splitter[role=separator]')].map((e) => ({
    orientation: e.getAttribute('aria-orientation'), now: Number(e.getAttribute('aria-valuenow')), min: Number(e.getAttribute('aria-valuemin')), max: Number(e.getAttribute('aria-valuemax')), tab: e.tabIndex,
  })));
  check('[8] separators: orientation, tabindex 0 and real min < now < max (not the library 0..100)',
    separators.length >= 2 && separators.every((s) => s.orientation && s.tab === 0 && s.min < s.now && s.now < s.max && !(s.min === 0 && s.max === 100)), JSON.stringify(separators));
  const strips = await page.evaluate(() => [...document.querySelectorAll('[role=tablist]')].filter((l) => l.querySelector('[id^=flexlayout-tabbutton-]')).map((l) => ({
    stops: [...l.querySelectorAll('[role=tab]')].filter((t) => t.tabIndex === 0).length, selected: [...l.querySelectorAll('[role=tab]')].filter((t) => t.getAttribute('aria-selected') === 'true').length,
  })));
  check('[8] every tab strip has one selected tab and one tab stop (roving tabindex)', strips.length >= 3 && strips.every((s) => s.selected === 1 && s.stops === 1), JSON.stringify(strips));
  check('[8] tabs name their panel (aria-controls) and each panel is labelled by its tab',
    await page.evaluate(() => [...document.querySelectorAll('[id^=flexlayout-tabbutton-]')].every((t) => t.getAttribute('aria-controls')) && [...document.querySelectorAll('[id^=flexlayout-tab-]')].every((p) => p.getAttribute('aria-labelledby'))));
  const first = page.locator('.dock-host [role=separator]').first();
  await first.focus();
  const v0 = Number(await first.getAttribute('aria-valuenow'));
  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(200);
  const v1 = Number(await first.getAttribute('aria-valuenow'));
  check('[8] a separator moves with ArrowRight and reports the new value', v1 > v0, `${v0} -> ${v1}`);
  const live = await page.locator('#workbench-live').count();
  check('[8] the dock has a polite live region for move announcements', live === 1 && (await page.locator('#workbench-live').getAttribute('aria-live')) === 'polite');
  await context.close();
}

if (wants(9)) {
  const { page, context } = await openPage(browser, base, 1440, 900);
  await openProject(page);
  await page.evaluate(() => window.__tesseraEditor.edit('/* edited */\n'));
  await page.waitForTimeout(300);
  const facts = () => page.evaluate(() => ({
    active: document.querySelector('.editor-tab-select[aria-selected="true"]')?.title ?? '',
    dirty: document.querySelectorAll('.editor-tab-dirty').length,
    text: window.__tesseraEditor.text(),
  }));
  const before = await facts();
  await via(page, "wb.commands.movePanelDirection('canvas', 'bottom')");
  await via(page, "wb.dispatch({kind:'close', panelId:'scene'})");
  await page.waitForTimeout(400);
  const changed = JSON.stringify(await model(page));
  await page.getByRole('menuitem', { name: 'View', exact: true }).first().click();
  await page.locator('[role=menu] [role^=menuitem]', { hasText: /^Reset Layout/ }).first().click();
  await page.waitForTimeout(600);
  const reset = await model(page);
  const after = await facts();
  check('[9] Reset Layout restores the default arrangement', JSON.stringify(reset) !== changed && groupOf(reset, 'canvas')?.id === 'g-preview' && reset.closed.length === 0 && reset.collapsed.join() === 'g-bottom');
  check('[9] and does not change the open file or its dirty state', before.dirty >= 1 && JSON.stringify(before) === JSON.stringify(after), JSON.stringify({ before: { ...before, text: before.text.length }, after: { ...after, text: after.text.length } }));
  await context.close();
}

console.log(`\n${state.failures === 0 ? 'all layout checks passed' : `${state.failures} layout check(s) failed`}`);
await browser.close();
close();
process.exit(state.failures === 0 ? 0 : 1);
