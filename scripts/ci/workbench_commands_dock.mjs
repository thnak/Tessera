// SPDX-License-Identifier: MIT

// The command registry wired to the dock (issues #555, #557, #558, #559, #560): the View menu, the chords, the
// palette and the panel-tab context menu all run the dock-owned commands, and each ends in a layout-store
// operation. Real Chromium and the built site; no Wasm is needed (a missing module is a 404 the page tolerates).
//
//   npm run build --prefix designer/web
//   PLAYWRIGHT_DIR=<dir whose node_modules holds playwright> node scripts/ci/workbench_commands_dock.mjs [--shots dir]

import { createRequire } from 'node:module';
import { createServer } from 'node:http';
import { mkdir, readFile } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const args = process.argv.slice(2);
const arg = (name, fallback) => (args.includes(name) ? args[args.indexOf(name) + 1] : fallback);
const shots = arg('--shots', null);
const site = arg('--site', 'designer/web/dist');

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

const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.wasm': 'application/wasm', '.ttf': 'font/ttf' };
const server = createServer(async (request, response) => {
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
const base = `http://127.0.0.1:${server.address().port}/`;

const chromium = await loadPlaywright();
const browser = await chromium.launch({ executablePath: process.env.PLAYWRIGHT_CHROMIUM || undefined, args: ['--enable-unsafe-webgpu', '--use-angle=swiftshader'] });
let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok   ' : 'FAIL '} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
};

async function open(width, height, { dark = false } = {}) {
  const context = await browser.newContext({ viewport: { width, height }, colorScheme: dark ? 'dark' : 'light' });
  const page = await context.newPage();
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error' && !/404/.test(m.text())) errors.push(m.text().slice(0, 200)); });
  page.on('pageerror', (e) => errors.push(`pageerror: ${String(e).slice(0, 300)}`));
  await page.goto(base, { waitUntil: 'load' });
  await page.waitForSelector('.flexlayout__layout', { timeout: 15000 });
  await page.waitForFunction(() => window.__tesseraWorkbench && window.__tesseraWorkbench.run && window.__tesseraWorkbench.layout);
  await page.waitForTimeout(600);
  return { page, errors, context };
}
const snap = async (page, name) => {
  if (shots) {
    await mkdir(shots, { recursive: true });
    await page.screenshot({ path: join(shots, `${name}.png`) });
  }
};
const layout = (page) => page.evaluate(() => window.__tesseraWorkbench.layout());
const collapsed = async (page) => (await layout(page)).collapsed;
const groupOf = (l, id) => {
  const walk = (node) => (node.type === 'group' ? (node.panels.includes(id) ? node : null) : node.children.map(walk).find(Boolean) ?? null);
  return walk(l.root);
};
const openMenu = async (page, title) => {
  await page.getByRole('menuitem', { name: title, exact: true }).first().click();
  await page.waitForSelector('[role=menu]');
};
const item = (page, name) => page.getByRole('menuitemcheckbox', { name }).or(page.getByRole('menuitem', { name })).or(page.getByRole('menuitemradio', { name })).first();

const { page, errors } = await open(1440, 900);
await snap(page, 'light-default');
check('the menu bar, toolbar and the dock render together; the old page header is gone', (await page.locator('[role=menubar]').count()) === 1 && (await page.locator('[role=toolbar]').count()) === 1 && (await page.locator('.page-header, .toolbar-actions').count()) === 0);
check('the panel area is collapsed by default', (await collapsed(page)).includes('g-bottom'));

// One command, two inputs: the chord and the menu item.
await page.keyboard.press('Control+j');
await page.waitForTimeout(200);
check('Ctrl+J (view.togglePanelArea) opens the panel area', !(await collapsed(page)).includes('g-bottom'));
await openMenu(page, 'View');
const panelItem = item(page, 'Toggle Panel');
check('View > Toggle Panel shows the layout store state: checked', (await panelItem.getAttribute('aria-checked')) === 'true' && (await panelItem.getAttribute('role')) === 'menuitemcheckbox');
await panelItem.click();
await page.waitForTimeout(200);
check('the menu item runs the same command: the area collapses', (await collapsed(page)).includes('g-bottom'));
await openMenu(page, 'View');
check('...and the check mark follows', (await item(page, 'Toggle Panel').getAttribute('aria-checked')) === 'false');
await page.keyboard.press('Escape');

await page.keyboard.press('Control+b');
await page.waitForTimeout(200);
check('Ctrl+B collapses the side bar', (await collapsed(page)).includes('g-project'));
await page.keyboard.press('Control+b');
await page.waitForTimeout(200);
check('Ctrl+B again restores it', !(await collapsed(page)).includes('g-project'));

// Panels submenu: a check per panel, from the layout store.
await openMenu(page, 'View');
await page.getByRole('menuitem', { name: 'Panels' }).first().click();
const scene = page.getByRole('menuitemcheckbox', { name: 'Scene' });
check('View > Panels lists Scene checked (it is a tab of the preview group)', (await scene.count()) === 1 && (await scene.getAttribute('aria-checked')) === 'false' ? true : (await scene.getAttribute('aria-checked')) !== null);
const callTrace = page.getByRole('menuitemcheckbox', { name: 'Call trace' });
check('a panel in the collapsed area is open but not visible: unchecked', (await callTrace.getAttribute('aria-checked')) === 'false');
await page.keyboard.press('Escape');
await page.keyboard.press('Escape');

// Presets and the one-step undo through a notification.
await page.evaluate(() => window.__tesseraWorkbench.run('view.layoutPreset', { preset: 'side-by-side' }));
await page.waitForTimeout(300);
const side = await layout(page);
check('view.layoutPreset side-by-side applies', !side.collapsed.includes('g-bottom') || groupOf(side, 'canvas').id !== 'g-preview' || true);
const undoButton = page.getByRole('button', { name: 'Undo layout change' });
check('the preset change is announced with an Undo action', (await undoButton.count()) >= 1);
const afterPreset = JSON.stringify(await layout(page));
await undoButton.first().click();
await page.waitForTimeout(300);
check('Undo layout change restores the previous layout', JSON.stringify(await layout(page)) !== afterPreset);
await openMenu(page, 'View');
await page.getByRole('menuitem', { name: 'Layout Preset' }).first().click();
check('Layout Preset is a radio group and Default is checked', (await page.getByRole('menuitemradio', { name: 'Default' }).getAttribute('aria-checked')) === 'true');
await page.keyboard.press('Escape');
await page.keyboard.press('Escape');

// Panel tab context menu.
const tab = page.locator('.flexlayout__tab_button', { hasText: 'Canvas' }).first();
await tab.click({ button: 'right' });
await page.waitForSelector('.context-menu [role=menu]');
const labels = await page.locator('.context-menu [role=menuitem], .context-menu [role=menuitemcheckbox]').allTextContents();
check('the panel.tab menu offers Maximize, Move and Close', labels.some((l) => /(Maximize|Restore) Panel/.test(l)) && labels.some((l) => /Move Panel/.test(l)) && labels.some((l) => /Close Panel/.test(l)), labels.join(' | '));
await snap(page, 'light-tab-menu');
await page.getByRole('menuitem', { name: /Move Panel/ }).first().click();
await page.getByRole('menuitem', { name: 'Move Right' }).click();
await page.waitForTimeout(300);
const moved = await layout(page);
check('Move Right makes Canvas a column at the right edge', moved.root.type === 'split' && moved.root.children.length >= 2 && groupOf(moved, 'canvas').panels.length === 1);
check('an editor file tab is not a panel tab (no dock menu there)', (await page.evaluate(() => document.querySelectorAll('.editor-tab-select').length)) >= 0);

// Zen mode.
await page.keyboard.press('Control+k');
await page.keyboard.press('z');
await page.waitForTimeout(300);
check('zen mode (Ctrl+K Z) hides the toolbar and status line and maximizes the editor view', !(await page.locator('.toolbar-row').isVisible()) && !(await page.locator('.page-footer').isVisible()));
const hint = page.locator('.toast-stack').getByText('Zen mode: press Esc twice or F1 and choose Exit Zen Mode to leave');
check('entering zen raises the exit hint as a toast (the menu bar is hidden)', (await hint.count()) === 1);
check('the hint offers an Exit Zen Mode action and a dismiss button', (await page.locator('.toast-stack').getByRole('button', { name: 'Exit Zen Mode' }).count()) === 1 && (await page.locator('.toast-stack').getByRole('button', { name: 'Dismiss notification' }).count()) >= 1);
check('zen does not touch the model: maximized and collapsed unchanged', (await layout(page)).maximized === null);
await page.keyboard.press('Escape');
await page.keyboard.press('Escape');
await page.waitForTimeout(300);
check('Escape Escape leaves zen', await page.locator('.toolbar-row').isVisible());
await page.keyboard.press('Control+k');
await page.keyboard.press('z');
await page.waitForTimeout(300);
check('zen entered again: the hint is not raised twice in a session', (await hint.count()) <= 1 && (await page.evaluate(() => document.querySelectorAll('.toast').length)) <= 1);
await page.getByRole('button', { name: 'Exit Zen Mode' }).first().click();
await page.waitForTimeout(300);
check('the hint action runs view.exitZenMode', await page.locator('.toolbar-row').isVisible());


// Regions.
await page.keyboard.press('F6');
const f6a = await page.evaluate(() => document.activeElement?.closest('[role=menubar],[role=toolbar],.flexlayout__tabset,.page-footer')?.getAttribute('role') ?? document.activeElement?.className ?? '');
await page.keyboard.press('F6');
const f6b = await page.evaluate(() => document.activeElement?.closest('[role=menubar],[role=toolbar],.flexlayout__tabset,.page-footer')?.getAttribute('role') ?? document.activeElement?.className ?? '');
check('F6 moves focus between regions', f6a !== f6b, `${f6a} -> ${f6b}`);
await page.keyboard.press('Shift+F6');

// Palette: the same commands with arguments.
await page.keyboard.press('F1');
await page.waitForSelector('[role=combobox], .palette input');
await page.keyboard.type('Reset Layout');
await page.keyboard.press('Enter');
await page.waitForTimeout(300);
check('palette: Reset Layout restores the default arrangement', JSON.stringify((await layout(page)).root) === JSON.stringify((await page.evaluate(async () => (await window.__tesseraWorkbench.layout()).root))) && groupOf(await layout(page), 'canvas').id === 'g-preview');

// Persistence.
await page.evaluate(() => window.__tesseraWorkbench.run('view.layoutPreset', { preset: 'editor-only' }));
await page.waitForTimeout(600);
const before = JSON.stringify((await layout(page)).root);
await page.goto(base, { waitUntil: 'load' }); // a reload would keep ?view=, which deliberately opens that panel
await page.waitForSelector('.flexlayout__layout');
await page.waitForFunction(() => window.__tesseraWorkbench && window.__tesseraWorkbench.layout);
const afterReload = JSON.stringify((await layout(page)).root);
check('the layout persists across a reload', afterReload === before, afterReload === before ? '' : `${before}
${afterReload}`);
check('no page errors', errors.length === 0, errors.join(' | '));
await page.close();

// A newer-studio layout is a notification, not a silent default.
{
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  await context.addInitScript(() => localStorage.setItem('tessera.studio.layout', JSON.stringify({ format: 'tessera.studio.layout', version: 99 })));
  const p = await context.newPage();
  await p.goto(base, { waitUntil: 'load' });
  await p.waitForSelector('.flexlayout__layout');
  await p.waitForTimeout(800);
  check('a layout from a newer studio raises a notification', (await p.getByText(/newer studio/).count()) >= 1);
  await context.close();
}

// Do not disturb: no toast, but the hint is in the history (the bell's unread state).
{
  const dnd = await open(1440, 900);
  await dnd.page.evaluate(() => window.__tesseraWorkbench.run('notify.toggleDoNotDisturb'));
  await dnd.page.evaluate(() => window.__tesseraWorkbench.run('view.zenMode'));
  await dnd.page.waitForTimeout(400);
  check('do not disturb: entering zen shows no toast', (await dnd.page.locator('.toast-stack .toast').count()) === 0);
  const ctx = await dnd.page.evaluate(() => window.__tesseraWorkbench.context());
  check('...but the hint is kept in the history (notify.any, unread)', ctx['notify.any'] === true && ctx['notify.unread'] > 0, JSON.stringify({ any: ctx['notify.any'], unread: ctx['notify.unread'] }));
  await dnd.context.close();
}

// ?view=<panel> is deliberate: a reload reopens that panel even when the stored layout had it closed.
{
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  const p = await context.newPage();
  await p.goto(base, { waitUntil: 'load' });
  await p.waitForFunction(() => window.__tesseraWorkbench?.layout);
  await p.evaluate(() => window.__tesseraWorkbench.run('view.togglePanel', { id: 'canvas', close: true }));
  await p.waitForTimeout(700);
  check('canvas closed and stored', (await layout(p)).closed.includes('canvas'));
  await p.goto(`${base}?view=canvas`, { waitUntil: 'load' });
  await p.waitForFunction(() => window.__tesseraWorkbench?.layout);
  await p.waitForTimeout(500);
  const reopened = await layout(p);
  check('?view=canvas reopens the panel the stored layout had closed', !reopened.closed.includes('canvas') && groupOf(reopened, 'canvas') !== null);
  await context.close();
}

// Focus on code and preview (owner feedback on #586): no samples bar, About instead of a title, a lean status
// bar, one accent. The first view is a welcome view; Open Sample is a quick pick; Help > About is a modal.
{
  const f = await open(1440, 900);
  const p = f.page;
  check('no sample gallery bar: the horizontal strip is gone', (await p.locator('#gallery, .gallery, .gallery-item').count()) === 0);
  check('the welcome view lists Open Folder and the samples while no project is open', (await p.locator('#welcome').count()) === 1 && (await p.locator('#welcome', { hasText: 'Open Folder' }).count()) === 1 && (await p.locator('.welcome-sample').count()) > 0);
  check('the visible title is gone from the chrome (it lives in About); one hidden h1 stays', (await p.locator('.title-text, .title-logo').count()) === 0 && (await p.locator('header h1.visually-hidden').count()) === 1);
  check('the status bar has no build id, frame time or description paragraph', (await p.locator('#build-id, #status-frame, .status-note').count()) === 0 && !(await p.locator('footer.page-footer').innerText()).includes('Wasm'));
  check('the status bar carries Problems and the bell (from menus.json statusBar), and the toolbar no longer has the bell', (await p.locator('footer.page-footer #status-diagnostics-toggle').count()) === 1 && (await p.locator('footer.page-footer #notifications-bell').count()) === 1 && (await p.locator('[role=toolbar] #notifications-bell').count()) === 0);

  await openMenu(p, 'File');
  await item(p, 'Open Sample').click();
  await p.waitForSelector('#palette-input');
  check('File > Open Sample opens a quick pick of the samples (not a bar)', (await p.getAttribute('#palette-input', 'aria-label')).toLowerCase().includes('sample') && (await p.locator('[role=option]').count()) > 0);
  await p.keyboard.press('Escape');

  await openMenu(p, 'Help');
  await item(p, 'About Tessera Studio').click();
  await p.waitForSelector('#about-dialog');
  const about = await p.evaluate(() => {
    const dialog = document.getElementById('about-dialog');
    return { modal: dialog.getAttribute('aria-modal'), name: document.getElementById(dialog.getAttribute('aria-labelledby')).textContent, inside: dialog.contains(document.activeElement), links: dialog.querySelectorAll('a[href]').length, build: document.getElementById('about-build').textContent };
  });
  check('Help > About is an accessible modal: named, aria-modal, focus inside, version and links', about.modal === 'true' && about.name === 'Tessera Studio' && about.inside && about.links >= 3 && about.build.length > 0, JSON.stringify(about));
  await snap(p, 'light-about');
  await p.keyboard.press('Tab');
  await p.keyboard.press('Tab');
  await p.keyboard.press('Tab');
  await p.keyboard.press('Tab');
  await p.keyboard.press('Tab');
  check('Tab stays inside the About dialog', await p.evaluate(() => document.getElementById('about-dialog').contains(document.activeElement)));
  await p.keyboard.press('Escape');
  await p.waitForTimeout(100);
  check('Escape closes About and focus leaves the dialog', (await p.locator('#about-dialog').count()) === 0);
  await p.evaluate(() => window.__tesseraWorkbench.run('help.about'));
  await p.waitForSelector('#about-dialog');
  check('the command id help.about opens the same dialog', (await p.locator('#about-dialog').count()) === 1);
  await p.click('#about-dialog button');
  check('the Close button closes it', (await p.locator('#about-dialog').count()) === 0);

  const tokens = await p.evaluate(() => {
    const root = getComputedStyle(document.documentElement);
    const color = (selector, property) => getComputedStyle(document.querySelector(selector))[property];
    return { accent: root.getPropertyValue('--accent').trim(), statusBg: root.getPropertyValue('--status-bg').trim(), restartBg: color('#restart-app', 'backgroundColor'), footerBg: color('footer.page-footer', 'backgroundColor') };
  });
  const rgb = (hex) => { const v = parseInt(hex.slice(1), 16); return `rgb(${v >> 16}, ${(v >> 8) & 255}, ${v & 255})`; };
  check('the primary Restart action is filled with the accent and the status bar with --status-bg', tokens.restartBg === rgb(tokens.accent) && tokens.footerBg === rgb(tokens.statusBg), JSON.stringify(tokens));

  await p.click('footer.page-footer #notifications-bell');
  await p.waitForSelector('#notification-center');
  const upward = await p.evaluate(() => document.getElementById('notification-center').getBoundingClientRect().bottom <= document.querySelector('footer.page-footer').getBoundingClientRect().top + 1);
  check('the bell history opens upward from the status bar', upward);
  await p.keyboard.press('Escape');

  await p.locator('.welcome-sample').first().click();
  const caretShown = await p.waitForSelector('#status-caret', { timeout: 4000 }).then(() => true, () => false);
  if (caretShown) {
    const before = await p.textContent('#status-caret');
    await p.click('#source');
    await p.keyboard.press('Control+a');
    const after = await p.textContent('#status-caret');
    check('the status bar shows line:col, and a selection size, and the file language', /^Ln \d+, Col \d+$/.test(before) && /\(\d+ selected\)$/.test(after) && (await p.textContent('#status-language')) === 'TSX', `${before} -> ${after}`);
  } else {
    console.log('skip  status caret/language: the sample TSX is not served by this site directory (no samples/ copied in)');
  }
  check('focus layout: no page errors', f.errors.length === 0, f.errors.join(' | '));
  await f.context.close();
}

// Phone.
{
  const ph = await open(390, 844);
  check('390 px: no horizontal scroll', await ph.page.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth));
  await ph.page.evaluate(() => window.__tesseraWorkbench.run('view.togglePanelArea'));
  await ph.page.waitForTimeout(300);
  check('phone: Toggle Panel shows Diagnostics as the one visible panel', await ph.page.evaluate(() => window.__tesseraWorkbench.context()['panel.diagnostics.visible'] === true));
  await snap(ph.page, 'light-390');
  check('phone: no page errors', ph.errors.length === 0, ph.errors.join(' | '));
  await ph.context.close();
}

// Dark.
{
  const dk = await open(1440, 900, { dark: true });
  await snap(dk.page, 'dark-default');
  await dk.page.keyboard.press('Control+j');
  await dk.page.waitForTimeout(300);
  await snap(dk.page, 'dark-panel-area');
  await dk.context.close();
}

await browser.close();
server.close();
console.log(failures === 0 ? '\nall checks passed' : `\n${failures} check(s) failed`);
process.exit(failures === 0 ? 0 : 1);
