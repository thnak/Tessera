// SPDX-License-Identifier: MIT
//
// The menu model's acceptance check (issue #557; `docs/specs/studio-workbench-v1.md` sections 3 and 4).
//
//   node scripts/ci/preview_workbench_menus.mjs
//
// Pure Node, no browser: `menus.json` is data and `menus.js` / `menu-nav.js` are plain modules. Proves:
//
//  1. Every menu item, toolbar entry and dynamic entry names a known command id, mnemonics are unique
//     letters of their titles, and every control the spec's section 4.1 mapped has a menu or toolbar home.
//  2. The built tree: reserved commands hide their items (and `Go` with them), separators fall between
//     groups only, radio and check kinds derive from the command's `checked` clause, a disabled item
//     carries its reason, chords come from the keymap, dynamic providers fill submenus.
//  3. The APG keyboard model (`menu-nav.js`): Left/Right across the bar, Up/Down with wrap, Home/End,
//     Enter/Space, submenus, Escape levels, Alt+letter, type-ahead, and a disabled item that is focusable
//     but inert.

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { assert, finish, test } from './mock_test_support.mjs';

const WORKBENCH = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'designer', 'web', 'src', 'workbench');
const load = (name) => import(pathToFileURL(resolve(WORKBENCH, name)).href);
const json = (name) => JSON.parse(readFileSync(resolve(WORKBENCH, name), 'utf8'));

const { createContext } = await load('context.js');
const { createRegistry } = await load('registry.js');
const { validateMenus, walkMenus, buildMenuBar, buildToolbar, buildItems } = await load('menus.js');
const { CLOSED, navigate, openByMnemonic, focusedItem } = await load('menu-nav.js');

const COMMANDS = json('commands.json').commands;
const MENUS = json('menus.json');

/** A registry whose live commands all have handlers, as the page's boot leaves it. */
function bootRegistry(contextValues = {}, records = COMMANDS) {
  const context = createContext({ 'project.open': true, 'project.busy': false, 'project.source': 'folder', 'project.folderSupported': true, 'run.enabled': true, 'canvas.available': true, ...contextValues });
  const registry = createRegistry(records, { context });
  for (const record of records) {
    if (record.status === 'live') {
      registry.register(record.id, () => {});
    }
  }
  return { registry, context };
}
const providers = {
  recent: () => [],
  components: () => [],
  profiles: () => [{ label: 'Custom size', args: { id: 'custom' } }, { label: 'Pixel 8', args: { id: 'pixel-8' } }],
  skins: () => [{ label: 'None', args: { value: 'none' } }, { label: 'Generic Phone', args: { value: 'generic-phone' } }],
  viewports: () => [{ label: 'Phone 390x844', args: { value: '390x844' } }, { label: 'Tablet 768x1024', args: { value: '768x1024' } }],
  dprs: () => [{ label: '1x', args: { value: '1' } }, { label: '2x', args: { value: '2' } }],
};
const labelsOf = (items) => items.filter((item) => item.kind !== 'separator').map((item) => item.label);

await test('menus.json: every item names a known command; mnemonics are unique letters', () => {
  assert.deepEqual(validateMenus(MENUS, COMMANDS), []);
  const bad = validateMenus({ menus: [{ id: 'x', title: 'Xy', mnemonic: 'Q', items: [{ command: 'no.such' }, { group: 'bad' }] }, { id: 'y', title: 'Yx', mnemonic: 'X', items: [] }, { id: 'z', title: 'Xz', mnemonic: 'X', items: [] }], toolbar: [{ command: 'also.missing' }] }, COMMANDS);
  assert.ok(bad.some((problem) => problem.includes("unknown command 'no.such'")));
  assert.ok(bad.some((problem) => problem.includes("unknown command 'also.missing'")));
  assert.ok(bad.some((problem) => problem.includes('mnemonic')));
  assert.ok(bad.some((problem) => problem.includes('used twice')));
  assert.ok(bad.some((problem) => problem.includes('names no command')));
  assert.ok(bad.some((problem) => problem.includes('bad group')));
});

await test('menus.json: every control of the spec mapping (section 4.1) has a home', () => {
  const homes = new Set();
  walkMenus(MENUS, (item) => {
    if (item.command) {
      homes.add(item.command);
    }
  });
  for (const command of [
    'file.openFolder', 'file.openZip', 'file.openRecent', 'file.samples', 'file.newFile', 'file.renameFile', 'file.deleteFile',
    'file.save', 'file.download', 'run.restart', 'run.togglePicker', 'run.toggleInspector', 'run.inputScript',
    'run.component', 'device.profile', 'device.setSkin', 'device.setViewport', 'device.setCustomSize', 'device.setDpr',
    'device.setFold', 'device.toggleKeepState', 'view.setTheme', 'view.palette',
  ]) {
    assert.ok(homes.has(command), `${command} is in no menu`);
  }
  const toolbar = MENUS.toolbar.map((entry) => entry.command ?? entry.widget);
  assert.deepEqual(toolbar, ['run.restart', 'file.save', 'deviceChip', 'paletteButton']);
  assert.equal(MENUS.toolbar[0].emphasis, 'primary', 'Restart is the one primary action');
  // The status bar is data too (owner feedback on #586): Problems, caret, language, bell, in this order.
  assert.deepEqual(MENUS.statusBar.map((entry) => entry.widget), ['problems', 'caret', 'language', 'bell']);
});

await test('build: six live menus, Go is hidden until a go.* command is live', () => {
  const { registry } = bootRegistry();
  const bar = buildMenuBar(MENUS, { registry, providers });
  assert.deepEqual(bar.map((menu) => menu.title), ['File', 'Edit', 'View', 'Run', 'Device', 'Help']);
  const live = COMMANDS.map((record) => (record.id === 'go.definition' ? { ...record, status: 'live' } : record));
  const next = bootRegistry({}, live);
  const withGo = buildMenuBar(MENUS, { registry: next.registry, providers });
  assert.deepEqual(withGo.map((menu) => menu.title), ['File', 'Edit', 'View', 'Go', 'Run', 'Device', 'Help']);
  assert.deepEqual(labelsOf(withGo.find((menu) => menu.id === 'go').items), ['Go to Definition']);
});

await test('build: reserved items are hidden; separators fall between groups only', () => {
  const { registry } = bootRegistry();
  const bar = buildMenuBar(MENUS, { registry, providers });
  const file = bar.find((menu) => menu.id === 'file');
  assert.deepEqual(labelsOf(file.items), ['Open Folder...', 'Open Zip...', 'Open Recent', 'Open Sample...', 'New File', 'Save', 'Download Project (.zip)', 'Rename File...', 'Delete File']);
  for (const menu of bar) {
    const kinds = menu.items.map((item) => item.kind);
    assert.notEqual(kinds[0], 'separator', menu.id);
    assert.notEqual(kinds.at(-1), 'separator', menu.id);
    kinds.forEach((kind, index) => assert.ok(!(kind === 'separator' && kinds[index + 1] === 'separator'), `${menu.id} doubled separator`));
  }
  assert.deepEqual(file.items.map((item) => item.kind), ['item', 'item', 'submenu', 'item', 'separator', 'item', 'item', 'item', 'separator', 'item', 'item']);
  const view = bar.find((menu) => menu.id === 'view');
  assert.ok(labelsOf(view.items).includes('Problems'), 'Problems is live now (the panel exists)');
  assert.ok(!labelsOf(view.items).includes('Source Control'), 'a reserved panel stays hidden');
  assert.ok(!labelsOf(bar.find((menu) => menu.id === 'edit').items).includes('Format Document'));
});

await test('build: chords come from the keymap, disabled items carry a reason', () => {
  const { registry, context } = bootRegistry({ 'project.source': 'sample' });
  let bar = buildMenuBar(MENUS, { registry, providers });
  const find = (id, label) => bar.find((menu) => menu.id === id).items.find((item) => item.label === label);
  assert.equal(find('file', 'Save').chord, 'Ctrl+S');
  assert.equal(find('file', 'Save').enabled, false);
  assert.match(find('file', 'Save').reason, /unavailable/);
  assert.equal(find('file', 'New File').chord, 'Ctrl+Alt+N');
  assert.equal(find('file', 'Open Folder...').chord, 'Ctrl+K Ctrl+O');
  assert.equal(find('edit', 'Find...').chord, 'Ctrl+F');
  assert.equal(find('run', 'Restart App').chord, 'Ctrl+Shift+F5');
  context.set({ 'project.source': 'folder' });
  bar = buildMenuBar(MENUS, { registry, providers });
  assert.equal(find('file', 'Save').enabled, true);
});

await test('build: radio and check kinds derive from the checked clause; dynamic providers fill submenus', () => {
  const { registry } = bootRegistry({ 'ui.theme': 'dark', 'device.viewport': '390x844', 'device.keepState': true, 'run.picker': false });
  const bar = buildMenuBar(MENUS, { registry, providers });
  const appearance = bar.find((menu) => menu.id === 'view').items.find((item) => item.label === 'Appearance');
  assert.deepEqual(appearance.items.map((item) => [item.label, item.kind, item.checked]), [['Theme: System', 'radio', false], ['Theme: Light', 'radio', false], ['Theme: Dark', 'radio', true]]);
  const device = bar.find((menu) => menu.id === 'device');
  const viewport = device.items.find((item) => item.label === 'Viewport');
  assert.deepEqual(viewport.items.filter((item) => item.kind === 'radio').map((item) => [item.label, item.checked]), [['Phone 390x844', true], ['Tablet 768x1024', false]]);
  assert.ok(viewport.items.some((item) => item.kind === 'separator'), 'presets and Custom Size are separate groups');
  assert.equal(device.items.find((item) => item.label === 'Keep State on Device Change').kind, 'check');
  assert.equal(device.items.find((item) => item.label === 'Keep State on Device Change').checked, true);
  assert.equal(bar.find((menu) => menu.id === 'run').items.find((item) => item.label === 'Picker').checked, false);
  const recent = bar.find((menu) => menu.id === 'file').items.find((item) => item.label === 'Open Recent');
  assert.equal(recent.kind, 'submenu');
  assert.equal(recent.enabled, false, 'an empty Open Recent stays visible but disabled');
  assert.ok(!labelsOf(bar.find((menu) => menu.id === 'run').items).includes('Component'), 'Component is hidden with one component');
  const multi = bootRegistry({ 'project.multiComponent': true });
  const withComponents = buildMenuBar(MENUS, { registry: multi.registry, providers: { ...providers, components: () => [{ label: 'App', args: { name: 'App' } }] } });
  assert.ok(labelsOf(withComponents.find((menu) => menu.id === 'run').items).includes('Component'));
});

await test('build: group order is first appearance, items sort by order within a group', () => {
  const { registry } = bootRegistry();
  const items = buildItems([
    { command: 'file.save', group: 'b@2' },
    { command: 'file.openZip', group: 'a@1' },
    { command: 'file.download', group: 'b@1' },
  ], { registry, keyPrefix: 't' });
  assert.deepEqual(items.map((item) => (item.kind === 'separator' ? '-' : item.label)), ['Download Project (.zip)', 'Save', '-', 'Open Zip...']);
});

await test('toolbar: only live commands, emphasis and widgets carried through', () => {
  const { registry } = bootRegistry();
  const toolbar = buildToolbar(MENUS, { registry });
  assert.deepEqual(toolbar.map((entry) => entry.command ?? entry.widget), ['run.restart', 'file.save', 'deviceChip', 'paletteButton']);
  assert.equal(toolbar[0].emphasis, 'primary');
  assert.equal(toolbar[0].chord, 'Ctrl+Shift+F5');
  assert.equal(toolbar[1].label, 'Save');
});

function sampleMenus() {
  const { registry } = bootRegistry({ 'project.source': 'sample' });
  return buildMenuBar(MENUS, { registry, providers });
}
const press = (menus, nav, ...keys) => keys.reduce((state, key) => navigate(menus, state.nav, key), { nav, effect: null });

await test('nav: the bar moves with Left/Right and wraps, Home/End jump', () => {
  const menus = sampleMenus();
  let result = press(menus, CLOSED, 'ArrowRight');
  assert.equal(result.nav.bar, 1);
  assert.equal(press(menus, CLOSED, 'ArrowLeft').nav.bar, menus.length - 1);
  assert.equal(press(menus, CLOSED, 'End').nav.bar, menus.length - 1);
  assert.equal(press(menus, press(menus, CLOSED, 'End').nav, 'Home').nav.bar, 0);
  assert.equal(press(menus, CLOSED, 'x').handled, false);
  assert.equal(press(menus, CLOSED, 'Escape').effect.type, 'exit');
});

await test('nav: Down/Enter/Space open the first item, Up the last; Esc closes to the bar', () => {
  const menus = sampleMenus();
  const down = press(menus, CLOSED, 'ArrowDown');
  assert.deepEqual([down.nav.open, down.nav.stack[0]], [true, 0]);
  assert.equal(press(menus, CLOSED, 'Enter').nav.open, true);
  assert.equal(press(menus, CLOSED, ' ').nav.open, true);
  const up = press(menus, CLOSED, 'ArrowUp');
  assert.equal(menus[0].items[up.nav.stack[0]].label, 'Delete File');
  const closed = press(menus, down.nav, 'Escape');
  assert.equal(closed.nav.open, false);
  assert.equal(closed.effect.type, 'close-bar');
  assert.equal(closed.nav.bar, 0);
});

await test('nav: Up/Down skip separators and wrap; Home/End; type-ahead', () => {
  const menus = sampleMenus();
  let nav = press(menus, CLOSED, 'ArrowDown').nav;
  nav = press(menus, nav, 'End').nav;
  assert.equal(focusedItem(menus, nav).label, 'Delete File');
  nav = press(menus, nav, 'ArrowDown').nav;
  assert.equal(focusedItem(menus, nav).label, 'Open Folder...', 'wraps to the first');
  nav = press(menus, nav, 'ArrowUp').nav;
  assert.equal(focusedItem(menus, nav).label, 'Delete File');
  nav = press(menus, nav, 'n').nav;
  assert.equal(focusedItem(menus, nav).label, 'New File');
  nav = press(menus, nav, 'd').nav;
  assert.ok(['Download Project (.zip)', 'Delete File'].includes(focusedItem(menus, nav).label));
  for (let i = 0; i < 12; i += 1) {
    nav = press(menus, nav, 'ArrowDown').nav;
    assert.notEqual(focusedItem(menus, nav).kind, 'separator');
  }
});

await test('nav: Right/Left switch menus, submenus open and close, Enter runs an enabled item', () => {
  const menus = sampleMenus();
  let nav = press(menus, CLOSED, 'ArrowDown').nav;
  nav = press(menus, nav, 'ArrowRight').nav;
  assert.deepEqual([nav.bar, nav.open], [1, true]);
  nav = press(menus, nav, 'ArrowLeft').nav;
  assert.equal(nav.bar, 0);
  nav = press(menus, nav, 'ArrowDown').nav;
  nav = press(menus, nav, 'ArrowDown').nav;
  assert.equal(focusedItem(menus, nav).label, 'Open Recent');
  assert.equal(press(menus, nav, 'Enter').nav.stack.length, 1, 'an empty submenu does not open');
  // Device > Skin has entries.
  const deviceBar = menus.findIndex((menu) => menu.id === 'device');
  nav = openByMnemonic(menus, 'D');
  assert.equal(nav.bar, deviceBar);
  nav = press(menus, nav, 'ArrowDown').nav;
  assert.equal(focusedItem(menus, nav).label, 'Skin');
  nav = press(menus, nav, 'ArrowRight').nav;
  assert.equal(nav.stack.length, 2);
  assert.equal(focusedItem(menus, nav).label, 'None');
  assert.equal(press(menus, nav, 'Escape').nav.stack.length, 1, 'Escape closes one level');
  assert.equal(press(menus, nav, 'ArrowLeft').nav.stack.length, 1);
  const run = press(menus, nav, 'Enter');
  assert.equal(run.effect.type, 'run');
  assert.deepEqual([run.effect.item.command, run.effect.item.args], ['device.setSkin', { value: 'none' }]);
  assert.equal(run.nav.open, false);
});

await test('nav: a disabled item is focusable and inert; Alt+letter opens by mnemonic; Tab closes', () => {
  const menus = sampleMenus();
  let nav = openByMnemonic(menus, 'f');
  assert.equal(nav.open, true);
  assert.equal(nav.bar, 0);
  nav = press(menus, nav, 'End').nav;
  for (let i = 0; i < 3; i += 1) {
    nav = press(menus, nav, 'ArrowUp').nav;
  }
  const focused = focusedItem(menus, nav);
  assert.equal(focused.label, 'Save', 'a sample project: Save is disabled');
  assert.equal(focused.enabled, false);
  const inert = press(menus, nav, 'Enter');
  assert.equal(inert.effect, null);
  assert.equal(inert.nav.open, true);
  assert.equal(openByMnemonic(menus, 'g'), null, 'Go is hidden, so Alt+G opens nothing');
  assert.equal(press(menus, nav, 'Tab').effect.type, 'close');
  assert.equal(press(menus, nav, 'Tab').nav.open, false);
});

finish('preview_workbench_menus');
