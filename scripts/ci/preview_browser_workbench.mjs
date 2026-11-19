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
    check('End goes to the last item', (await focusedLabel(page)).startsWith('Copy Share Link'));
    await page.keyboard.press('ArrowDown');
    check('Down wraps to the first item', (await focusedLabel(page)).startsWith('Open Folder'));
    await page.keyboard.press('ArrowUp');
    check('Up wraps to the last item', (await focusedLabel(page)).startsWith('Copy Share Link'));
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
    await page.focus('#share');
    await page.keyboard.press('Alt+h');
    await page.keyboard.press('Escape');
    check('Escape closes onto the bar button first (APG)', (await focusedId(page)) === 'menubar-help');
    await page.keyboard.press('Escape');
    check('a second Escape on the bar restores the previous focus', (await focusedId(page)) === 'share');
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

  // --- screenshots (light, dark, 390) -----------------------------------------------------------------------
  if (shots !== null) {
    await mkdir(shots, { recursive: true });
    for (const [name, width, height, scheme] of [['light', 1440, 900, 'light'], ['dark', 1440, 900, 'dark'], ['phone', 390, 844, 'light']]) {
      const { page, context } = await open(width, height, scheme);
      await page.screenshot({ path: join(shots, `workbench-${name}-closed.png`) });
      await page.click('.menubar-item');
      await page.screenshot({ path: join(shots, `workbench-${name}-menu.png`) });
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
