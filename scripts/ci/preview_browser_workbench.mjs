// SPDX-License-Identifier: MIT
//
// The workbench chrome in a real browser (issue #557, #558; `docs/design/studio-workbench-core.md`).
// Developer-run, like the other `preview_browser_*` scripts: it needs Playwright and Chromium, which the
// repository does not depend on.
//
//   npm install playwright && npx playwright install chromium     (in a scratch directory)
//   node scripts/ci/preview_browser_workbench.mjs --site designer/web/dist [--shots out-dir] [--chromium path-to-chrome]
//
// It serves a built site (`npm run build` in `designer/web`), needs no Wasm module (the chrome does not), and
// proves what the Node gates cannot: the WAI-ARIA menu bar with real focus, the device chip popover, the
// toolbar's single tab stop, the palette and quick open, and that a menu item runs the same command as its
// shortcut. With `--shots` it also writes light, dark and 390 px screenshots.

import { createServer } from 'node:http';
import { mkdir, readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';

import { writeZip } from '../../designer/web/studio-zip.js';

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.wasm': 'application/wasm', '.ttf': 'font/ttf', '.txt': 'text/plain; charset=utf-8' };
const argument = (name, fallback) => {
  const index = process.argv.indexOf(name);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
};
const site = argument('--site', 'designer/web/dist');
const shots = argument('--shots', null);
const executablePath = argument('--chromium', undefined);

let chromium;
try {
  ({ chromium } = await import('playwright'));
} catch {
  console.error('playwright is not installed. This script is developer-run on purpose (no repository dependency):\n  npm install playwright && npx playwright install chromium');
  process.exit(2);
}

const server = createServer(async (request, response) => {
  let path = normalize(decodeURIComponent(new URL(request.url, 'http://127.0.0.1').pathname));
  if (path === '/' || path === '\\') path = '/index.html';
  try {
    const body = await readFile(join(site, path));
    response.writeHead(200, { 'content-type': MIME[extname(path).toLowerCase()] ?? 'application/octet-stream', 'cache-control': 'no-store' });
    response.end(body);
  } catch {
    response.writeHead(404, { 'content-type': 'text/plain' });
    response.end('not found');
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch({ executablePath });
let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok   ' : 'FAIL '} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
};
const open = async (width = 1440, height = 900, colorScheme = 'light') => {
  const context = await browser.newContext({ viewport: { width, height }, colorScheme });
  const page = await context.newPage();
  const problems = [];
  page.on('pageerror', (error) => problems.push(String(error)));
  await page.goto(base + '/', { waitUntil: 'load' });
  await page.waitForSelector('.menubar-item');
  return { page, context, problems };
};
const focusedLabel = (page) => page.evaluate(() => document.activeElement?.textContent?.trim() ?? '');
const focusedId = (page) => page.evaluate(() => document.activeElement?.id ?? '');
const expanded = (page, id) => page.getAttribute(`#${id}`, 'aria-expanded');

try {
  // --- ARIA structure -----------------------------------------------------------------------------
  {
    const { page, context, problems } = await open();
    const bar = await page.evaluate(() => {
      const items = [...document.querySelectorAll('[role="menubar"] > .menubar-slot > [role="menuitem"]')];
      return { roles: items.map((node) => node.textContent.trim()), tabbable: items.filter((node) => node.tabIndex === 0).length, haspopup: items.every((node) => node.getAttribute('aria-haspopup') === 'menu') };
    });
    check('menubar: six menus, aria-haspopup, one tab stop', bar.roles.join(',') === 'File,Edit,View,Run,Device,Help' && bar.haspopup && bar.tabbable === 1, JSON.stringify(bar));

    // --- keyboard on the bar -------------------------------------------------------------------------
    await page.focus('#menubar-file');
    await page.keyboard.press('ArrowRight');
    check('bar: Right moves to Edit (roving focus)', (await focusedId(page)) === 'menubar-edit');
    await page.keyboard.press('ArrowLeft');
    await page.keyboard.press('ArrowLeft');
    check('bar: Left wraps to Help', (await focusedId(page)) === 'menubar-help');
    await page.keyboard.press('Home');
    check('bar: Home goes to File', (await focusedId(page)) === 'menubar-file');
    await page.keyboard.press('End');
    check('bar: End goes to Help', (await focusedId(page)) === 'menubar-help');
    await page.keyboard.press('Home');

    // --- open, move, close ------------------------------------------------------------------------
    await page.keyboard.press('ArrowDown');
    check('Down opens the menu and focuses the first item', (await expanded(page, 'menubar-file')) === 'true' && (await focusedLabel(page)).startsWith('Open Folder'));
    const roles = await page.evaluate(() => [...document.querySelectorAll('#menu-file [role]')].map((node) => node.getAttribute('role')));
    check('menu: role=menu with menuitems and separators', roles.includes('menuitem') && roles.includes('separator'));
    await page.keyboard.press('ArrowDown');
    check('Down moves to the next item', (await focusedLabel(page)).startsWith('Open Zip'));
    await page.keyboard.press('End');
    check('End goes to the last item', (await focusedLabel(page)).startsWith('Delete File'));
    await page.keyboard.press('ArrowDown');
    check('Down wraps to the first item', (await focusedLabel(page)).startsWith('Open Folder'));
    await page.keyboard.press('ArrowUp');
    check('Up wraps to the last item', (await focusedLabel(page)).startsWith('Delete File'));
    await page.keyboard.press('n');
    check('type-ahead: n focuses New File', (await focusedLabel(page)).startsWith('New File'));
    await page.keyboard.press('Escape');
    check('Escape closes the menu and returns focus to the bar button', (await expanded(page, 'menubar-file')) === 'false' && (await focusedId(page)) === 'menubar-file');

    // --- disabled-but-focusable ---------------------------------------------------------------------
    await page.keyboard.press('Enter');
    await page.keyboard.press('s');
    const save = await page.evaluate(() => ({ label: document.activeElement.textContent.trim(), disabled: document.activeElement.getAttribute('aria-disabled'), role: document.activeElement.getAttribute('role') }));
    check('a disabled item is focusable and aria-disabled', save.label.startsWith('Save') && save.disabled === 'true' && save.role === 'menuitem', JSON.stringify(save));
    await page.keyboard.press('Enter');
    check('Enter on a disabled item does nothing (menu stays open)', (await expanded(page, 'menubar-file')) === 'true');
    await page.keyboard.press('Escape');

    // --- Alt+letter and Left/Right between menus -------------------------------------------------------
    await page.focus('#editor-host, body');
    await page.keyboard.press('Alt+v');
    check('Alt+V opens View', (await expanded(page, 'menubar-view')) === 'true');
    await page.keyboard.press('ArrowRight');
    check('Right in an open menu moves to the next menu (Run)', (await expanded(page, 'menubar-run')) === 'true');
    await page.keyboard.press('ArrowLeft');
    check('Left moves back (View)', (await expanded(page, 'menubar-view')) === 'true');

    // --- submenu and radio -----------------------------------------------------------------------------
    await page.keyboard.press('Escape');
    await page.keyboard.press('Alt+d');
    await page.keyboard.press('ArrowDown');
    check('Device > Skin is a submenu', (await focusedLabel(page)).startsWith('Skin') && (await page.evaluate(() => document.activeElement.getAttribute('aria-haspopup'))) === 'menu');
    await page.keyboard.press('ArrowRight');
    const radio = await page.evaluate(() => ({ role: document.activeElement.getAttribute('role'), checked: document.activeElement.getAttribute('aria-checked'), label: document.activeElement.textContent.trim() }));
    check('submenu items are menuitemradio with aria-checked', radio.role === 'menuitemradio' && radio.checked === 'true' && radio.label.includes('None'), JSON.stringify(radio));
    await page.keyboard.press('Escape');
    check('Escape closes one level (submenu), the menu stays open', (await expanded(page, 'menubar-device')) === 'true' && (await focusedLabel(page)).startsWith('Skin'));
    await page.keyboard.press('Escape');
    check('a second Escape closes the menu', (await expanded(page, 'menubar-device')) === 'false');

    // --- a menu item runs the command ---------------------------------------------------------------------
    await page.keyboard.press('Alt+v');
    await page.keyboard.press('End');
    const appearance = await focusedLabel(page);
    check('View ends with Appearance', appearance.startsWith('Appearance'), appearance);
    await page.keyboard.press('ArrowRight');
    await page.keyboard.press('End');
    await page.keyboard.press('Enter');
    check('Appearance > Dark sets the theme through its command', (await page.evaluate(() => document.documentElement.dataset.theme)) === 'dark');
    await page.evaluate(() => window.__tesseraWorkbench.run('view.setTheme', { value: 'system' }));

    // --- the same command from the menu and from its chord ------------------------------------------------
    const prompts = [];
    page.on('dialog', async (dialog) => {
      prompts.push(dialog.message());
      await dialog.dismiss();
    });
    await page.keyboard.press('Control+Alt+n');
    await page.waitForTimeout(100);
    await page.keyboard.press('Alt+f');
    await page.keyboard.press('n');
    await page.keyboard.press('Enter');
    await page.waitForTimeout(100);
    check('File > New File and Ctrl+Alt+N run the same command (two identical prompts)', prompts.length === 2 && prompts[0] === prompts[1] && prompts[0].startsWith('New file path'), JSON.stringify(prompts));

    // --- focus returns to where it was -----------------------------------------------------------------------
    await page.focus('#restart-app');
    await page.keyboard.press('Alt+h');
    await page.keyboard.press('Escape');
    check('Escape closes onto the bar button first (APG)', (await focusedId(page)) === 'menubar-help');
    await page.keyboard.press('Escape');
    check('a second Escape on the bar restores the previous focus', (await focusedId(page)) === 'restart-app');
    check('no uncaught page error', problems.length === 0, problems.join(' | '));
    await context.close();
  }

  // --- the device chip ------------------------------------------------------------------------------------
  {
    const { page, context } = await open();
    const before = await page.textContent('#device-chip .chip-text');
    await page.focus('#device-chip');
    await page.keyboard.press('Enter');
    check('chip: Enter opens the Device menu popover', (await expanded(page, 'device-chip')) === 'true' && (await page.locator('#menu-device-chip').count()) === 1);
    await page.keyboard.press('Escape');
    check('chip: Escape returns focus to the chip', (await expanded(page, 'device-chip')) === 'false' && (await focusedId(page)) === 'device-chip');
    await page.click('#device-chip');
    await page.click('#menu-device-chip >> text=Viewport');
    await page.click('text=Phone');
    const after = await page.textContent('#device-chip .chip-text');
    check('chip: picking a viewport updates the chip text', before !== after && after.includes('390'), `${before} -> ${after}`);
    // --- toolbar: one tab stop, arrows rove -----------------------------------------------------------------
    const stops = await page.evaluate(() => [...document.querySelectorAll('[role="toolbar"][aria-label="Studio actions"] [data-roving]')].filter((node) => node.tabIndex === 0).length);
    check('toolbar: exactly one tab stop', stops === 1, String(stops));
    await page.focus('[role="toolbar"][aria-label="Studio actions"] [data-roving][tabindex="0"]');
    await page.keyboard.press('ArrowRight');
    check('toolbar: Right moves focus along the toolbar', (await focusedId(page)) !== 'restart-app');
    await context.close();
  }

  // --- the command palette, Quick Open, Go to Line, context menus ------------------------------------------
  {
    const { page, context, problems } = await open();
    const encoder = new TextEncoder();
    const lines = Array.from({ length: 60 }, (_, index) => `// line ${index + 1}`).join('\n');
    const files = {
      'tessera.json': '{"schemaVersion":1,"app":{"name":"zip-demo","entry":"ui/Home.tsx","namespace":"zip_demo"}}',
      'ui/Home.tsx': `export function Home(): Node {\n  return <View />;\n}\n${lines}\n`,
      'ui/Settings.tsx': 'export function Settings(): Node {\n  return <View />;\n}\n',
      'styles/app.css': '.page { width: 320px; }\n',
    };
    const buffer = Buffer.from(writeZip(Object.entries(files).map(([path, text]) => ({ path, bytes: encoder.encode(text) }))));
    await page.locator('input[type=file]').setInputFiles({ name: 'my-app.zip', mimeType: 'application/zip', buffer });
    await page.waitForSelector('#project-name');
    const palette = {
      open: () => page.waitForSelector('#palette-input'),
      options: () => page.$$eval('#palette-list [role="option"]', (nodes) => nodes.map((node) => node.textContent.trim())),
      active: () =>
        page.evaluate(() => {
          const input = document.getElementById('palette-input');
          const id = input?.getAttribute('aria-activedescendant');
          return id ? document.getElementById(id)?.textContent.trim() : null;
        }),
      closed: async () => (await page.locator('#palette-input').count()) === 0,
    };

    // --- Ctrl+Shift+P and F1 ----------------------------------------------------------------------------
    await page.focus('#restart-app');
    await page.keyboard.press('Control+Shift+P');
    await palette.open();
    const aria = await page.evaluate(() => {
      const input = document.getElementById('palette-input');
      const list = document.getElementById('palette-list');
      return {
        role: input.getAttribute('role'),
        expanded: input.getAttribute('aria-expanded'),
        controls: input.getAttribute('aria-controls') === list.id,
        listRole: list.getAttribute('role'),
        options: list.querySelectorAll('[role="option"]').length,
        focused: document.activeElement === input,
        value: input.value,
        activedescendant: input.hasAttribute('aria-activedescendant'),
      };
    });
    check('Ctrl+Shift+P opens the palette: combobox + listbox, input focused, ">" prefilled', aria.role === 'combobox' && aria.expanded === 'true' && aria.controls && aria.listRole === 'listbox' && aria.options > 10 && aria.focused && aria.value === '>' && aria.activedescendant, JSON.stringify(aria));
    await page.keyboard.press('ArrowDown');
    const moved = await palette.active();
    const selected = await page.$$eval('#palette-list [role="option"][aria-selected="true"]', (nodes) => nodes.map((node) => node.textContent.trim()));
    check('Down moves aria-activedescendant to the next option', moved !== null && selected.length === 1 && selected[0] === moved, String(moved));
    await page.keyboard.press('Escape');
    check('Escape closes the palette and restores focus', (await palette.closed()) && (await focusedId(page)) === 'restart-app');
    await page.keyboard.press('F1');
    await palette.open();
    check('F1 opens the palette too', true);
    await page.keyboard.press('Escape');

    // --- a command without arguments -------------------------------------------------------------------------
    await page.keyboard.press('Control+Shift+P');
    await palette.open();
    await page.keyboard.type('toggle theme');
    check('typing filters and ranks (Toggle Theme first)', (await palette.options())[0].startsWith('View: Toggle Theme'), (await palette.options()).slice(0, 3).join(' | '));
    await page.keyboard.press('Enter');
    check('Enter runs it (the theme cycles to light) and closes', (await palette.closed()) && (await page.evaluate(() => document.documentElement.dataset.theme)) === 'light');
    await page.evaluate(() => window.__tesseraWorkbench.run('view.setTheme', { value: 'system' }));

    // --- recent commands lead an empty query ----------------------------------------------------------------------
    await page.keyboard.press('Control+Shift+P');
    await palette.open();
    check('the command just used is first in the recent list', (await palette.options())[0].startsWith('View: Toggle Theme'), (await palette.options())[0]);
    await page.keyboard.press('Escape');

    // --- a command with a choice argument -----------------------------------------------------------------------------
    await page.keyboard.press('Control+Shift+P');
    await palette.open();
    await page.keyboard.type('device: viewport');
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => document.querySelector('#palette-list [role="option"]'));
    const choices = await palette.options();
    check('a command with args opens a choice prompt over the provider choices', choices.length === 4 && choices[0].includes('320'), choices.join(' | '));
    await page.keyboard.press('ArrowDown');
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => document.querySelector('#device-chip .chip-text')?.textContent.includes('390'));
    check('picking the choice runs the command with it (the chip shows 390x844)', await palette.closed());

    // --- typed arguments, validated -----------------------------------------------------------------------------------
    await page.keyboard.press('Control+Shift+P');
    await palette.open();
    await page.keyboard.type('custom size');
    await page.keyboard.press('Enter');
    await page.keyboard.type('10');
    await page.keyboard.press('Enter');
    const invalid = await page.evaluate(() => ({ invalid: document.getElementById('palette-input').getAttribute('aria-invalid'), status: document.getElementById('palette-status').textContent }));
    check('an out-of-range number is refused with a message, the prompt stays', invalid.invalid === 'true' && /120/.test(invalid.status), JSON.stringify(invalid));
    await page.keyboard.press('Control+A');
    await page.keyboard.type('700');
    await page.keyboard.press('Enter');
    await page.keyboard.type('500');
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => document.querySelector('#device-chip .chip-text')?.textContent.includes('700'));
    check('two typed arguments run Custom Size (the chip shows 700x500)', (await page.textContent('#device-chip .chip-text')).includes('700×500'));

    // --- Quick Open (Ctrl+P), file at line, Go to Line (Ctrl+G) -------------------------------------------------------------
    await page.focus('#restart-app');
    await page.keyboard.press('Control+p');
    await palette.open();
    check('Ctrl+P opens Quick Open with the project files', (await palette.options()).includes('ui/Home.tsx') && (await page.inputValue('#palette-input')) === '');
    await page.keyboard.type('sett');
    check('fuzzy search ranks Settings first', (await palette.options())[0] === 'ui/Settings.tsx', (await palette.options()).join(' | '));
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => document.querySelector('.editor-tab-select[aria-selected="true"]')?.title === 'ui/Settings.tsx');
    check('Enter opens the file in the editor', true);
    await page.focus('#restart-app');
    await page.keyboard.press('Control+p');
    await palette.open();
    await page.keyboard.type('Home.tsx:25');
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => document.querySelector('.editor-tab-select[aria-selected="true"]')?.title === 'ui/Home.tsx');
    await page.waitForTimeout(300);
    const caretLine = await page.evaluate(() => document.querySelector('.cm-activeLine')?.textContent ?? '');
    check('name:line opens the file and puts the caret on that line', caretLine.includes('line 22'), caretLine);
    await page.keyboard.press('Control+g');
    await palette.open();
    check('Ctrl+G opens Go to Line (":" prefilled) even with the editor focused', (await page.inputValue('#palette-input')) === ':');
    await page.keyboard.type('2');
    await page.keyboard.press('Enter');
    await page.waitForTimeout(300);
    const goneTo = await page.evaluate(() => document.querySelector('.cm-activeLine')?.textContent ?? '');
    check('Enter goes to the line', (await palette.closed()) && goneTo.includes('return'), goneTo);

    // --- context menus ---------------------------------------------------------------------------------------------------------
    await page.click('#project-tree [role="treeitem"]', { button: 'right' });
    await page.waitForSelector('.context-menu [role="menu"]');
    const labels = await page.$$eval('.context-menu [role="menuitem"]', (nodes) => nodes.map((node) => node.textContent.trim()));
    check('right-click on a file opens the tree.item menu (New File, Rename, Delete)', labels.some((label) => label.startsWith('New File')) && labels.some((label) => label.startsWith('Rename')) && labels.some((label) => label.startsWith('Delete')), labels.join(' | '));
    await page.keyboard.press('Escape');
    check('Escape closes the context menu', (await page.locator('.context-menu').count()) === 0);
    await page.focus('#project-tree [role="treeitem"]');
    await page.keyboard.press('Shift+F10');
    await page.waitForSelector('.context-menu [role="menu"]');
    check('Shift+F10 opens the tree menu from the keyboard, focus on its first item', (await page.evaluate(() => document.activeElement?.getAttribute('role'))) === 'menuitem');
    await page.keyboard.press('Escape');
    await page.click('.cm-content', { button: 'right' });
    await page.waitForSelector('.context-menu [role="menu"]');
    const editorLabels = await page.$$eval('.context-menu [role="menuitem"]', (nodes) => nodes.map((node) => node.textContent.trim()));
    check('right-click in the editor opens the editor menu (Undo ... Go to Line)', editorLabels.some((label) => label.startsWith('Undo')) && editorLabels.some((label) => label.startsWith('Go to Line')), editorLabels.join(' | '));
    await page.keyboard.press('Escape');
    check('no uncaught page error (palette and menus)', problems.length === 0, problems.join(' | '));
    await context.close();
  }

  // --- notifications (issue #559) ---------------------------------------------------------------------------
  {
    const { page, context, problems } = await open();
    const live = await page.evaluate(() => {
      const region = document.getElementById('notification-live');
      return { live: region?.getAttribute('aria-live'), inDocument: region !== null, role: document.getElementById('notifications')?.getAttribute('role') };
    });
    check('the toast region is in the document before any toast, aria-live polite', live.inDocument && live.live === 'polite' && live.role === 'region', JSON.stringify(live));
    const bellAria = await page.evaluate(() => {
      const bell = document.getElementById('notifications-bell');
      return { label: bell.getAttribute('aria-label'), popup: bell.getAttribute('aria-haspopup'), expanded: bell.getAttribute('aria-expanded') };
    });
    check('the bell is a labelled button with a popup', bellAria.label === 'Notifications' && bellAria.popup === 'dialog' && bellAria.expanded === 'false', JSON.stringify(bellAria));

    await page.evaluate(() => window.__tesseraWorkbench.run('notify.show', { message: 'Layout reset', actionLabel: 'Toggle theme', actionCommand: 'view.cycleTheme' }));
    await page.waitForSelector('#notification-live .toast');
    const toast = await page.evaluate(() => {
      const node = document.querySelector('#notification-live .toast');
      return { text: node.querySelector('.toast-message').textContent, actions: [...node.querySelectorAll('.toast-action')].map((button) => button.textContent), inLive: document.getElementById('notification-live').contains(node) };
    });
    check('a toast with an action appears inside the polite live region', toast.text === 'Layout reset' && toast.actions.join() === 'Toggle theme' && toast.inLive, JSON.stringify(toast));
    check('the bell counts it as unread', (await page.getAttribute('#notifications-bell', 'aria-label')) === 'Notifications, 1 unread');
    await page.click('#notification-live .toast-action');
    check('the action runs its command (the theme cycles) and the toast goes away', (await page.evaluate(() => document.documentElement.dataset.theme)) === 'light' && (await page.locator('#notification-live .toast').count()) === 0);
    await page.evaluate(() => window.__tesseraWorkbench.run('view.setTheme', { value: 'system' }));

    await page.focus('#notifications-bell');
    await page.keyboard.press('Enter');
    await page.waitForSelector('#notification-center');
    const history = await page.$$eval('#notification-center .nc-message', (nodes) => nodes.map((node) => node.textContent));
    check('the bell history lists it, and opening marks it read', history.includes('Layout reset') && (await page.getAttribute('#notifications-bell', 'aria-label')) === 'Notifications');
    await page.keyboard.press('Escape');
    check('Escape closes the history and returns focus to the bell', (await page.locator('#notification-center').count()) === 0 && (await focusedId(page)) === 'notifications-bell');

    await page.evaluate(() => window.__tesseraWorkbench.run('notify.show', { message: 'Same thing', key: 'dup' }));
    await page.evaluate(() => window.__tesseraWorkbench.run('notify.show', { message: 'Same thing', key: 'dup' }));
    check('the same key twice is one toast with a counter', (await page.locator('#notification-live .toast').count()) === 1 && (await page.textContent('#notification-live .toast-count')) === '2');
    await page.click('#notification-live .toast-dismiss');

    await page.evaluate(() => window.__tesseraWorkbench.run('notify.toggleDoNotDisturb'));
    await page.evaluate(() => window.__tesseraWorkbench.run('notify.show', { message: 'Quiet one', severity: 'warning' }));
    await page.waitForTimeout(150);
    check('do not disturb: no toast, but the bell counts it', (await page.locator('#notification-live .toast').count()) === 0 && (await page.getAttribute('#notifications-bell', 'aria-label')).includes('2 unread'));
    await page.click('#notifications-bell');
    await page.waitForSelector('#notification-center');
    const quiet = await page.$$eval('#notification-center .nc-message', (nodes) => nodes.map((node) => node.textContent));
    check('do not disturb keeps the history', quiet.includes('Quiet one') && (await page.getAttribute('#notifications-dnd', 'aria-pressed')) === 'true', quiet.join(' | '));
    await page.click('#notifications-dnd');
    await page.click('#notifications-clear');
    check('Clear all empties the history', (await page.locator('#notification-center .nc-item').count()) === 0);
    await page.keyboard.press('Escape');

    // The project note is a notification now: opening a zip says so through the service.
    const encoder = new TextEncoder();
    const buffer = Buffer.from(writeZip(Object.entries({ 'tessera.json': '{"schemaVersion":1,"app":{"name":"zip-demo","entry":"ui/Home.tsx","namespace":"zip_demo"}}', 'ui/Home.tsx': 'export function Home(): Node {\n  return <View />;\n}\n' }).map(([path, text]) => ({ path, bytes: encoder.encode(text) }))));
    await page.locator('input[type=file]').setInputFiles({ name: 'notes.zip', mimeType: 'application/zip', buffer });
    await page.waitForSelector('#project-name');
    await page.waitForSelector('#notification-live .toast');
    const toasted = await page.$$eval('#notification-live .toast-message', (nodes) => nodes.map((node) => node.textContent));
    check('what the project says is announced as a toast, and #project-note no longer has its own live region', toasted.some((text) => /archive|zip|opened|loaded/i.test(text)) && (await page.getAttribute('#project-note', 'role')) === null, toasted.join(' | '));
    check('no uncaught page error (notifications)', problems.length === 0, problems.join(' | '));
    await context.close();
  }

  // --- screenshots (light, dark, 390) -----------------------------------------------------------------------
  if (shots !== null) {
    await mkdir(shots, { recursive: true });
    for (const [name, width, height, scheme] of [['light', 1440, 900, 'light'], ['dark', 1440, 900, 'dark'], ['phone', 390, 844, 'light']]) {
      const { page, context } = await open(width, height, scheme);
      await page.screenshot({ path: join(shots, `workbench-${name}-closed.png`) });
      await page.click('.menubar-item');
      await page.screenshot({ path: join(shots, `workbench-${name}-menu.png`) });
      await page.keyboard.press('Escape');
      await page.keyboard.press('Escape');
      await page.keyboard.press('Control+Shift+P');
      await page.waitForSelector('#palette-input');
      await page.keyboard.type('device');
      await page.screenshot({ path: join(shots, `workbench-${name}-palette.png`) });
      await page.keyboard.press('Escape');
      await page.evaluate(() => window.__tesseraWorkbench.run('notify.show', { message: 'Layout reset', actionLabel: 'Undo', actionCommand: 'view.cycleTheme' }));
      await page.evaluate(() => window.__tesseraWorkbench.run('notify.show', { message: 'Saved 2 files', severity: 'info' }));
      await page.waitForSelector('#notification-live .toast');
      await page.click('#notifications-bell');
      await page.waitForSelector('#notification-center');
      await page.screenshot({ path: join(shots, `workbench-${name}-notifications.png`) });
      await page.keyboard.press('Escape');
      const overflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
      check(`${name}: no horizontal scroll`, !overflow);
      await context.close();
    }
  }
} finally {
  await browser.close();
  server.close();
}
console.log(failures === 0 ? '\nworkbench browser check: all passed' : `\nworkbench browser check: ${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
