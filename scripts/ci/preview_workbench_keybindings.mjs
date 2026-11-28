// SPDX-License-Identifier: MIT
//
// The keybinding overrides gate (issue #565, slice W8; `docs/specs/studio-workbench-v1.md` section 2.3).
//
//   node scripts/ci/preview_workbench_keybindings.mjs
//
// Pure Node, no browser and no build: `designer/web/src/workbench/keybindings.js` is plain ES modules. It proves:
//
//  1. Validation: a good document, the array shorthand, and a clear refusal for each bad shape (unknown or
//     retired command, malformed or browser-reserved chord, bad or unknown-key when, newer version, not JSON,
//     oversize). A refusal is all-or-nothing.
//  2. Merge: rebinding replaces the default chord (the old chord no longer fires, the new one does), clearing
//     unbinds, resetting restores, and the bindings survive a serialize and validate round trip.
//  3. Conflicts: the rows list the commands sharing a chord, and `collisionsFor` warns before a chord is taken.
//  4. Storage: a rebind works "after reload" (save then load into a fresh registry), a throwing store, a missing
//     store and a corrupt value all fall back to the defaults without throwing, and an invalid stored document
//     is not overwritten by loading it.
//  5. The recorder: `chordOfEvent` makes portable `Mod+` text and ignores a bare modifier.
//  6. The shipped defaults: `view.openSettings` is Mod+, and `view.openKeybindings` is Mod+K Mod+S, and the
//     records still validate.

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { assert, finish, test } from './mock_test_support.mjs';

const WORKBENCH = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'designer', 'web', 'src', 'workbench');
const load = (name) => import(pathToFileURL(resolve(WORKBENCH, name)).href);
const COMMANDS = JSON.parse(readFileSync(resolve(WORKBENCH, 'commands.json'), 'utf8')).commands;

const kb = await load('keybindings.js');
const { createRegistry, validateRecords } = await load('registry.js');
const { createContext } = await load('context.js');

const ev = (key, extra = {}) => ({ key, code: '', ctrlKey: false, altKey: false, shiftKey: false, metaKey: false, ...extra });
const byId = (id) => COMMANDS.find((record) => record.id === id);
const errorsOf = (input) => {
  const result = kb.validateOverrides(input, COMMANDS);
  assert.equal(result.ok, false, JSON.stringify(input));
  return result.errors.join(' | ');
};

/** A registry over the shipped records where every live command has a no-op handler. */
function registryWith(overrides) {
  const registry = createRegistry(COMMANDS, { context: createContext({}), overrides });
  for (const record of COMMANDS) {
    if (record.status === 'live') registry.register(record.id, () => {});
  }
  return registry;
}
const resolveKey = (registry, event) => registry.keymap().resolve(event, [], {});

class FakeStorage {
  constructor() { this.map = new Map(); }
  getItem(key) { return this.map.has(key) ? this.map.get(key) : null; }
  setItem(key, value) { this.map.set(key, value); }
  removeItem(key) { this.map.delete(key); }
}
const throwing = {
  getItem() { throw new Error('blocked'); },
  setItem() { throw new Error('blocked'); },
  removeItem() { throw new Error('blocked'); },
};

await test('validate: a versioned document and the array shorthand are accepted', () => {
  const doc = { format: kb.KEYBINDINGS_FORMAT, version: 1, bindings: [{ key: 'Mod+Shift+L', command: 'view.zenMode' }, { key: 'Mod+B', command: '-view.toggleSideBar' }] };
  const result = kb.validateOverrides(JSON.stringify(doc), COMMANDS);
  assert.equal(result.ok, true);
  assert.equal(result.overrides.length, 2);
  assert.equal(kb.validateOverrides(doc.bindings, COMMANDS).ok, true);
  assert.equal(kb.validateOverrides({ version: 1, bindings: [] }, COMMANDS).ok, true);
});

await test('validate: each bad shape is refused with a sentence that names it', () => {
  assert.match(errorsOf('not json'), /not JSON/);
  assert.match(errorsOf('{}'), /version must be 1/);
  assert.match(errorsOf({ version: 7, bindings: [] }), /newer studio/);
  assert.match(errorsOf({ format: 'other', version: 1, bindings: [] }), /format must be/);
  assert.match(errorsOf({ version: 1, bindings: 'x' }), /array of bindings/);
  assert.match(errorsOf([{ key: 'Mod+K' }]), /bindings\[0\]: expected/);
  assert.match(errorsOf([{ key: 'Mod+K', command: 'no.suchCommand' }]), /unknown command 'no.suchCommand'/);
  assert.match(errorsOf([{ key: 'Mod+K', command: '-no.suchCommand' }]), /unknown command/);
  assert.match(errorsOf([{ key: 'Mod+Shift+', command: 'view.zenMode' }]), /malformed chord/);
  assert.match(errorsOf([{ key: 'Mod+T', command: 'view.zenMode' }]), /reserved by the browser/);
  assert.match(errorsOf([{ key: 'Mod+K', command: 'view.zenMode', when: 'a &&' }]), /when 'a &&'/);
  assert.match(errorsOf([{ key: 'Mod+K', command: 'view.zenMode', when: 'nobody.publishes' }]), /unknown context key/);
  assert.match(errorsOf('['.padEnd(kb.MAX_KEYBINDINGS_BYTES + 1, ' ')), /larger than/);
  const many = Array.from({ length: kb.MAX_OVERRIDES + 1 }, () => ({ key: 'Mod+K', command: 'view.zenMode' }));
  assert.match(errorsOf(many), /the limit is/);
});

await test('validate: a retired command is refused with its replacement; one bad entry refuses the whole document', () => {
  const retired = COMMANDS.find((record) => record.status === 'removed');
  if (retired !== undefined) {
    assert.match(errorsOf([{ key: 'Mod+K', command: retired.id }]), /was retired/);
  }
  const text = errorsOf([{ key: 'Mod+Shift+L', command: 'view.zenMode' }, { key: 'Mod+K', command: 'no.suchCommand' }, { key: 'Mod+', command: 'view.zenMode' }]);
  assert.match(text, /bindings\[1\]/);
  assert.match(text, /bindings\[2\]/);
});

await test('merge: rebinding replaces the default chord, clearing unbinds, resetting restores', () => {
  const record = byId('view.toggleSideBar');
  assert.ok(record.keybinding.default, 'the test relies on a default chord');
  const rebound = kb.setBinding([], record, 'Mod+Shift+L');
  let registry = registryWith(rebound);
  assert.equal(resolveKey(registry, ev('L', { ctrlKey: true, shiftKey: true })).id, 'view.toggleSideBar');
  const old = resolveKey(registry, ev('B', { ctrlKey: true }));
  assert.notEqual(old.id, 'view.toggleSideBar', 'the old default no longer fires');
  // Rebinding again leaves one user chord, not two.
  const again = kb.setBinding(rebound, record, 'Mod+Shift+K');
  assert.equal(again.filter((entry) => entry.command === 'view.toggleSideBar').length, 1);
  registry = registryWith(kb.clearBinding(again, record));
  assert.equal(registry.keymap().chordFor('view.toggleSideBar'), '');
  registry = registryWith(kb.resetBinding(again, 'view.toggleSideBar'));
  assert.equal(registry.keymap().chordFor('view.toggleSideBar'), 'Ctrl+B');
  // Another command's overrides survive.
  const mixed = kb.resetBinding([...kb.setBinding([], byId('view.zenMode'), 'Mod+Shift+L'), ...rebound], 'view.toggleSideBar');
  assert.equal(mixed.every((entry) => entry.command.replace('-', '') === 'view.zenMode'), true);
});

await test('merge: the exported text validates back to the same overrides', () => {
  const overrides = kb.setBinding(kb.setBinding([], byId('view.zenMode'), 'Mod+Shift+L'), byId('view.toggleSideBar'), 'Alt+1', 'layout.zen');
  const text = kb.serializeOverrides(overrides);
  assert.equal(JSON.parse(text).version, 1);
  const back = kb.validateOverrides(text, COMMANDS);
  assert.equal(back.ok, true, JSON.stringify(back));
  assert.deepEqual(back.overrides, overrides);
});

await test('conflicts: the rows list commands sharing a chord, and collisionsFor warns before the chord is taken', () => {
  const overrides = kb.setBinding([], byId('view.zenMode'), 'Mod+B');
  const inputs = { commands: COMMANDS, overrides, isActive: () => true };
  const rows = kb.keybindingRows(inputs);
  const zen = rows.find((row) => row.id === 'view.zenMode');
  const side = rows.find((row) => row.id === 'view.toggleSideBar');
  assert.equal(zen.source, 'user');
  assert.deepEqual(zen.conflicts, ['view.toggleSideBar']);
  assert.deepEqual(side.conflicts, ['view.zenMode']);
  assert.deepEqual(kb.collisionsFor({ commands: COMMANDS, overrides: [], isActive: () => true }, 'view.zenMode', 'Ctrl+B'), ['view.toggleSideBar']);
  assert.deepEqual(kb.collisionsFor({ commands: COMMANDS, overrides: [] }, 'view.toggleSideBar', 'Mod+B'), []);
  assert.deepEqual(kb.collisionsFor({ commands: COMMANDS, overrides: [] }, 'view.zenMode', 'garbage+'), []);
  const clean = kb.keybindingRows({ commands: COMMANDS, overrides: [], isActive: () => true });
  assert.equal(clean.find((row) => row.id === 'view.openSettings').chord, 'Ctrl+,');
  assert.equal(clean.filter((row) => row.conflicts.length > 0).length, 0, 'the shipped defaults have no overlapping conflicts');
  // F2 in the tree and F2 in the editor are scoped apart: shared, not a conflict.
  assert.deepEqual(clean.find((row) => row.id === 'file.renameFile').shared, ['edit.renameSymbol']);
});

await test('rows: sources are default, user, removed and unbound; commands without a handler are not listed', () => {
  const rows = kb.keybindingRows({ commands: COMMANDS, overrides: kb.clearBinding([], byId('view.toggleSideBar')), isActive: () => true });
  assert.equal(rows.find((row) => row.id === 'view.toggleSideBar').source, 'removed');
  assert.equal(rows.find((row) => row.id === 'view.problems').source, 'default');
  assert.equal(rows.find((row) => row.id === 'view.history').source, 'unbound');
  assert.equal(kb.keybindingRows({ commands: COMMANDS, overrides: [], isActive: (id) => id !== 'view.history' }).some((row) => row.id === 'view.history'), false);
});

await test('storage: a rebind survives a reload (save, then a fresh registry loads it)', () => {
  const storage = new FakeStorage();
  assert.equal(kb.saveOverrides(storage, kb.setBinding([], byId('view.zenMode'), 'Mod+Shift+L')), true);
  const loaded = kb.loadOverrides(storage, COMMANDS);
  assert.equal(loaded.notice, null);
  const registry = registryWith(loaded.overrides);
  assert.equal(resolveKey(registry, ev('L', { ctrlKey: true, shiftKey: true })).id, 'view.zenMode');
  kb.saveOverrides(storage, []);
  assert.equal(storage.getItem(kb.KEYBINDINGS_STORAGE_KEY), null, 'no overrides clears the key');
});

await test('storage: a throwing, missing or corrupt store falls back to the defaults and never throws', () => {
  assert.deepEqual(kb.loadOverrides(throwing, COMMANDS), { overrides: [], notice: null });
  assert.deepEqual(kb.loadOverrides(null, COMMANDS), { overrides: [], notice: null });
  assert.equal(kb.saveOverrides(throwing, [{ key: 'Mod+K', command: 'view.zenMode' }]), false);
  assert.equal(kb.saveOverrides(null, []), false);
  for (const bad of ['{', '"x"', '{"version":9,"bindings":[]}', '[{"key":"Mod+T","command":"view.zenMode"}]']) {
    const storage = new FakeStorage();
    storage.setItem(kb.KEYBINDINGS_STORAGE_KEY, bad);
    const loaded = kb.loadOverrides(storage, COMMANDS);
    assert.deepEqual(loaded.overrides, [], bad);
    assert.match(loaded.notice, /could not be read/);
    assert.equal(storage.getItem(kb.KEYBINDINGS_STORAGE_KEY), bad, 'loading never rewrites the stored text');
  }
});

await test('import: an invalid import leaves the stored overrides unchanged', () => {
  const storage = new FakeStorage();
  const good = kb.setBinding([], byId('view.zenMode'), 'Mod+Shift+L');
  kb.saveOverrides(storage, good);
  const before = storage.getItem(kb.KEYBINDINGS_STORAGE_KEY);
  const attempt = kb.validateOverrides('[{"key":"Mod+K","command":"nope.nope"}]', COMMANDS);
  assert.equal(attempt.ok, false);
  // The page saves only on ok; the contract is that a refusal carries no overrides to save.
  assert.equal('overrides' in attempt, false);
  assert.equal(storage.getItem(kb.KEYBINDINGS_STORAGE_KEY), before);
});

await test('recorder: chordOfEvent writes portable text and ignores a bare modifier', () => {
  assert.equal(kb.chordOfEvent(ev('l', { ctrlKey: true, shiftKey: true })), 'Mod+Shift+L');
  assert.equal(kb.chordOfEvent(ev('l', { metaKey: true }), 'mac'), 'Mod+L');
  assert.equal(kb.chordOfEvent(ev('l', { ctrlKey: true }), 'mac'), 'Ctrl+L');
  assert.equal(kb.chordOfEvent(ev('l', { metaKey: true })), 'Meta+L');
  assert.equal(kb.chordOfEvent(ev('F1')), 'F1');
  assert.equal(kb.chordOfEvent(ev(',', { ctrlKey: true })), 'Mod+,');
  assert.equal(kb.chordOfEvent(ev('+', { ctrlKey: true })), 'Mod++');
  assert.equal(kb.chordOfEvent(ev('Control', { ctrlKey: true })), null);
  assert.equal(kb.chordOfEvent(ev('Shift', { shiftKey: true })), null);
  for (const key of ['Mod+,', 'Mod++', 'Mod+Shift+ARROWLEFT', 'Mod+SPACE']) {
    assert.notEqual(kb.validateOverrides([{ key, command: 'view.zenMode' }], COMMANDS).ok, undefined);
  }
  assert.equal(kb.validateOverrides([{ key: kb.chordOfEvent(ev('ArrowLeft', { ctrlKey: true, shiftKey: true })), command: 'view.zenMode' }], COMMANDS).ok, true);
  assert.equal(kb.validateOverrides([{ key: kb.chordOfEvent(ev(' ', { ctrlKey: true })), command: 'view.zenMode' }], COMMANDS).ok, true);
});

await test('defaults: Settings is Mod+, and the keyboard shortcuts editor is Mod+K Mod+S; records validate', () => {
  assert.equal(byId('view.openSettings').keybinding.default, 'Mod+,');
  assert.equal(byId('view.openKeybindings').keybinding.default, 'Mod+K Mod+S');
  assert.deepEqual(validateRecords(COMMANDS), []);
  const registry = registryWith([]);
  assert.equal(resolveKey(registry, ev(',', { ctrlKey: true })).id, 'view.openSettings');
  const first = registry.keymap().resolve(ev('k', { ctrlKey: true }), [], {});
  assert.equal(first.status, 'pending');
  assert.equal(registry.keymap().resolve(ev('s', { ctrlKey: true }), first.pending, {}).id, 'view.openKeybindings');
});

finish('workbench keybindings');
