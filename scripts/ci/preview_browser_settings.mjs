#!/usr/bin/env node
/**
 * Verify the studio's settings dialog and keybinding overrides in a real browser (issue #565, slice W8 of #516).
 *
 * Developer-run, not a CI gate (it needs Chromium). The pure parts (override merge, conflict detection, import
 * validation, storage fallback; layout import/export and saved presets) are Node gates:
 * `preview_workbench_keybindings.mjs` and `workbench_layout.mjs`.
 *
 * Usage:
 *   node scripts/ci/preview_browser_settings.mjs [--site designer/web/dist] [--shots dir]
 *
 * What it checks (1440x900):
 *   1  Ctrl+, opens the Settings dialog (role dialog, modal), Escape closes it and focus returns;
 *   2  rebinding a command with the recorder makes the new chord work, retires the old one, and both survive a reload;
 *   3  a chord two commands share is shown as a conflict on both rows;
 *   4  an invalid import is refused with a sentence and leaves the stored overrides and the working chord unchanged;
 *      a valid pasted import applies;
 *   5  with localStorage throwing, the defaults work, the dialog opens, a rebind applies to the session and says it
 *      was not stored, and there is no console error;
 *   6  layout: Export fills the JSON box, an invalid layout import changes nothing, a saved layout can be applied and
 *      deleted, and a valid import applies with one Undo;
 *   7  the theme picker sets the theme through the command.
 */

import { mkdir } from 'node:fs/promises';
import { join } from 'node:path';

import { argument, loadChromium, openPage, reporter, serve } from './studio_browser.mjs';

const site = argument('--site', 'designer/web/dist');
const shots = argument('--shots', null);
const chromium = await loadChromium();
const { base, close } = await serve(site);
console.log(`testing ${base}`);
const browser = await chromium.launch({ executablePath: process.env.PLAYWRIGHT_CHROMIUM || undefined });
const { state, check } = reporter();

const KEYS = 'tessera.studio.keybindings';
const context_ = (page) => page.evaluate(() => window.__tesseraWorkbench.context());
const sideBar = async (page) => (await context_(page))['layout.sideBar'];
const stored = (page) => page.evaluate((key) => localStorage.getItem(key), KEYS);
const row = (page, id) => page.locator(`#kb-table tr[data-command="${id}"]`);

async function openSettings(page, tab = 'Keyboard shortcuts') {
  await page.keyboard.press('Control+,');
  await page.waitForSelector('#settings-dialog');
  await page.getByRole('tab', { name: tab }).click();
}
async function closeSettings(page) {
  await page.keyboard.press('Escape');
  await page.waitForSelector('#settings-dialog', { state: 'detached' });
}
/** Rebinds through the dialog: records `chord` by pressing it in the editor field. */
async function rebind(page, title, chord) {
  await page.getByRole('button', { name: `Change shortcut for ${title}`, exact: true }).click();
  await page.getByRole('textbox', { name: `New shortcut for ${title}`, exact: true }).press(chord);
  await page.getByRole('button', { name: 'Set', exact: true }).click();
}
const snap = async (page, name) => {
  if (shots) {
    await mkdir(shots, { recursive: true });
    await page.screenshot({ path: join(shots, `${name}.png`) });
  }
};

// ---- 1 and 2: open, rebind, reload ------------------------------------------------------------------------------

{
  const { page, errors, context } = await openPage(browser, base, 1440, 900);
  await page.focus('#flexlayout-tabbutton-canvas');
  await openSettings(page);
  const dialog = page.getByRole('dialog', { name: 'Settings' });
  check('[1] Ctrl+, opens the Settings dialog: a modal dialog named Settings', (await dialog.count()) === 1 && (await dialog.getAttribute('aria-modal')) === 'true');
  await snap(page, 'settings-keybindings');
  await closeSettings(page);
  check('[1] Escape closes it and focus returns to where it was', (await page.evaluate(() => document.activeElement?.id)) === 'flexlayout-tabbutton-canvas');
  await page.keyboard.press('Control+k');
  await page.keyboard.press('Control+s');
  check('[1] Ctrl+K Ctrl+S opens the keyboard shortcuts', (await page.locator('#settings-dialog').count()) === 1 && (await page.locator('#settings-tab-keybindings').getAttribute('aria-selected')) === 'true');

  check('[2] before: Ctrl+B toggles the side bar', (await sideBar(page)) === true);
  await rebind(page, 'Toggle Side Bar', 'Control+Shift+L');
  check('[2] the row shows the new chord and marks it as yours', ((await row(page, 'view.toggleSideBar').locator('.kb-chord').textContent()) ?? '') === 'Ctrl+Shift+L' && /Yours/.test((await row(page, 'view.toggleSideBar').textContent()) ?? ''));
  await closeSettings(page);
  await page.keyboard.press('Control+b');
  await page.waitForTimeout(200);
  check('[2] the old chord (Ctrl+B) no longer fires', (await sideBar(page)) === true);
  await page.keyboard.press('Control+Shift+l');
  await page.waitForTimeout(200);
  check('[2] the new chord (Ctrl+Shift+L) toggles the side bar', (await sideBar(page)) === false);
  check('[2] the override is stored as a versioned document', /"version": 1/.test((await stored(page)) ?? '') && /-view.toggleSideBar/.test((await stored(page)) ?? ''));
  await page.reload({ waitUntil: 'load' });
  await page.waitForSelector('.flexlayout__layout');
  await page.waitForTimeout(800);
  const before = await sideBar(page);
  await page.keyboard.press('Control+Shift+l');
  await page.waitForTimeout(200);
  check('[2] after a reload the rebound chord still works and Ctrl+B still does not',
    (await sideBar(page)) === !before && (await (async () => { const was = await sideBar(page); await page.keyboard.press('Control+b'); await page.waitForTimeout(200); return (await sideBar(page)) === was; })()));
  check('no console errors', errors.length === 0, errors.slice(0, 3).join(' | ') || 'none');
  await context.close();
}

// ---- 3 conflicts, 4 import ----------------------------------------------------------------------------------------

{
  const { page, context } = await openPage(browser, base, 1440, 900);
  await openSettings(page);
  check('[3] the shipped defaults report no conflict', /No shortcut conflicts/.test((await page.locator('#kb-summary').textContent()) ?? ''));
  await rebind(page, 'Zen Mode', 'Control+j');
  const zen = (await row(page, 'view.zenMode').textContent()) ?? '';
  const panel = (await row(page, 'view.togglePanelArea').textContent()) ?? '';
  check('[3] a chord two commands share is a conflict on both rows', /Conflicts with Toggle Panel/.test(zen) && /Conflicts with Zen Mode/.test(panel), zen + ' | ' + panel);
  check('[3] the message names the other command and the summary counts it', /also used by/.test((await page.locator('#kb-outcome').textContent()) ?? '') && !/No shortcut conflicts/.test((await page.locator('#kb-summary').textContent()) ?? ''));
  await snap(page, 'settings-conflict');
  await page.locator('#kb-reset-all').click();

  await rebind(page, 'Toggle Side Bar', 'Control+Shift+l');
  const good = await stored(page);
  await page.locator('#keybindings-json').fill('[{"key":"Mod+K","command":"no.suchCommand"},{"key":"Mod+T","command":"view.zenMode"}]');
  await page.locator('#keybindings-import-text').click();
  const message = (await page.locator('#keybindings-outcome').textContent()) ?? '';
  check('[4] an invalid import is refused with a sentence that names the problems', /Nothing was imported/.test(message) && /unknown command 'no.suchCommand'/.test(message) && /reserved by the browser/.test(message), message);
  check('[4] and the refusal is an alert', (await page.locator('#keybindings-outcome').getAttribute('role')) === 'alert');
  check('[4] the stored overrides are unchanged', (await stored(page)) === good);
  await closeSettings(page);
  await page.keyboard.press('Control+Shift+l');
  await page.waitForTimeout(200);
  check('[4] and the working chord still works', (await sideBar(page)) === false);
  await openSettings(page);
  await page.locator('#keybindings-json').fill('{"format":"tessera-studio-keybindings","version":1,"bindings":[{"key":"Mod+Shift+Y","command":"view.zenMode"}]}');
  await page.locator('#keybindings-import-text').click();
  await closeSettings(page);
  await page.keyboard.press('Control+Shift+y');
  await page.waitForTimeout(300);
  check('[4] a valid pasted import applies at once (Ctrl+Shift+Y enters zen mode)', (await context_(page))['layout.zen'] === true);
  await context.close();
}

// ---- 5 throwing storage ------------------------------------------------------------------------------------------

{
  const { page, errors, context } = await openPage(browser, base, 1440, 900, {
    init: () => {
      const refuse = () => { throw new Error('storage blocked'); };
      Storage.prototype.getItem = refuse;
      Storage.prototype.setItem = refuse;
      Storage.prototype.removeItem = refuse;
    },
  });
  await page.keyboard.press('Control+b');
  await page.waitForTimeout(200);
  check('[5] with storage throwing, the default chord works', (await sideBar(page)) === false);
  await openSettings(page);
  await rebind(page, 'Toggle Side Bar', 'Control+Shift+l');
  check('[5] a rebind still applies to the session and says it was not stored', /would not store/.test((await page.locator('#kb-outcome').textContent()) ?? ''), (await page.locator('#kb-outcome').textContent()) ?? '');
  await closeSettings(page);
  await page.keyboard.press('Control+Shift+l');
  await page.waitForTimeout(200);
  check('[5] and the new chord works in the session', (await sideBar(page)) === true);
  check('[5] no console errors', errors.length === 0, errors.slice(0, 3).join(' | ') || 'none');
  await context.close();
}

// ---- 6 layout, 7 theme -------------------------------------------------------------------------------------------

{
  const { page, context } = await openPage(browser, base, 1440, 900);
  const layout = () => page.evaluate(() => JSON.stringify(window.__tesseraWorkbench.layout()));
  await openSettings(page, 'Layout');
  await page.locator('#layout-export').click();
  const exported = await page.locator('#layout-json').inputValue();
  check('[6] Export puts the layout JSON in the box', JSON.parse(exported).format === 'tessera.studio.layout', exported.slice(0, 40));
  const original = await layout();
  await page.locator('#layout-json').fill('{"format":"tessera.studio.layout","version":99}');
  await page.locator('#layout-import-text').click();
  check('[6] an invalid layout import is refused and the layout is unchanged', /Nothing was imported/.test((await page.locator('#layout-outcome').textContent()) ?? '') && (await layout()) === original, (await page.locator('#layout-outcome').textContent()) ?? '');
  await page.locator('#layout-preset-name').fill('Mine');
  await page.locator('#layout-preset-save').click();
  check('[6] a saved layout is listed', (await page.locator('#layout-presets li[data-preset="Mine"]').count()) === 1);
  await page.evaluate(() => window.__tesseraWorkbench.commands.movePanelDirection('canvas', 'bottom'));
  const moved = await layout();
  await page.getByRole('button', { name: 'Apply layout Mine' }).click();
  await page.waitForTimeout(300);
  check('[6] applying it brings the saved arrangement back', moved !== original && (await layout()) === original);
  await page.locator('#layout-json').fill(moved);
  await page.locator('#layout-import-text').click();
  await page.waitForTimeout(300);
  check('[6] a valid layout import applies', (await layout()) === moved);
  await page.getByRole('button', { name: 'Delete layout Mine' }).click();
  check('[6] a saved layout can be deleted', (await page.locator('#layout-presets').count()) === 0);
  await page.reload({ waitUntil: 'load' });
  await page.waitForSelector('.flexlayout__layout');
  await page.waitForTimeout(600);
  await openSettings(page, 'Layout');
  check('[6] and stays deleted after a reload', (await page.locator('#layout-preset-empty').count()) === 1);
  await page.getByRole('tab', { name: 'Appearance' }).click();
  await page.getByLabel('Dark').check();
  await page.waitForTimeout(300);
  check('[7] the theme picker sets the theme through the command', (await context_(page))['ui.theme'] === 'dark');
  await snap(page, 'settings-appearance');
  await context.close();
}

console.log(`\n${state.failures === 0 ? 'all settings checks passed' : `${state.failures} settings check(s) failed`}`);
await browser.close();
close();
process.exit(state.failures === 0 ? 0 : 1);
