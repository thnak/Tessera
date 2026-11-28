// SPDX-License-Identifier: MIT
//
// The workbench-core acceptance check (issues #555, #557, #558; `docs/specs/studio-workbench-v1.md`).
//
//   node scripts/ci/preview_workbench.mjs
//
// Pure Node, no browser and no build: `designer/web/src/workbench/` is plain ES modules plus JSON data, so
// this imports the shipped source. What it proves:
//
//  1. The when-clause evaluator: truth table, unknown keys are false, malformed clauses never throw.
//  2. The command records: valid, ids unique, every clause key is a published context key, no chord is one
//     the browser reserves (the named fallbacks Ctrl+Alt+N and Alt+W are used instead).
//  3. Id equality: a live command without a handler, a handler without a record and a reserved command with
//     a handler all fail; ids are append-only against the committed `command-ids.json`.
//  4. `run()`: the result codes, argument checks, disabled-with-reason, and a throwing handler isolated.
//  5. The keybinding resolver: chords, `when`, most specific wins, user override beats default, removal,
//     reserved-chord rejection of an override, and chord prefix fallbacks.
//  6. The core imports no `react`, `react-dom`, `@codemirror/*`, dock library or DOM global.
//
// The menu model and the palette have their own gates: `preview_workbench_menus.mjs` and
// `preview_workbench_palette.mjs`.

import { readFileSync, readdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { assert, finish, test } from './mock_test_support.mjs';

const WORKBENCH = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'designer', 'web', 'src', 'workbench');
const load = (name) => import(pathToFileURL(resolve(WORKBENCH, name)).href);
const json = (name) => JSON.parse(readFileSync(resolve(WORKBENCH, name), 'utf8'));

const { evaluate, parseClause, clauseError, specificityOf } = await load('when.js');
const { createContext, isKnownContextKey } = await load('context.js');
const { createRegistry, validateRecords, checkIntegrity } = await load('registry.js');
const { createKeymap, parseChord, chordProblem, formatChord } = await load('keys.js');

const COMMANDS = json('commands.json').commands;
const ev = (key, extra = {}) => ({ key, code: '', ctrlKey: false, altKey: false, shiftKey: false, metaKey: false, ...extra });

await test('when: truth table, unknown keys are false', () => {
  const ctx = { a: true, b: false, n: 2, s: 'folder' };
  const table = [
    ['a', true], ['b', false], ['!a', false], ['!b', true], ['a && b', false], ['a || b', true],
    ['a && !b', true], ['(a || b) && !b', true], ["s == 'folder'", true], ["s != 'folder'", false],
    ['n == 2', true], ['n != 3', true], ['missing', false], ['!missing', true], ['missing == true', false],
    ['a == a', true], ['true', true], ['false', false], ['!(a && b)', true], ['b || b || a', true],
  ];
  for (const [clause, expected] of table) {
    assert.equal(evaluate(clause, ctx), expected, clause);
  }
});

await test('when: malformed clauses never throw and evaluate false', () => {
  for (const bad of ['', 'a &&', '(a', 'a b', "s == 'x", 'a @ b', '&& a', 'a ==']) {
    assert.notEqual(clauseError(bad), null, bad);
    assert.equal(evaluate(bad, { a: true }), false, bad);
  }
  assert.equal(parseClause('a && b').ok, true);
  assert.equal(specificityOf(null), 0);
  assert.equal(specificityOf('a'), 1);
  assert.equal(specificityOf('a && !b && c == 1'), 3);
});

await test('commands.json: valid records, clause keys known, no browser-reserved chord', () => {
  assert.deepEqual(validateRecords(COMMANDS), []);
  assert.ok(COMMANDS.length > 50);
  assert.equal(isKnownContextKey('panel.canvas.open'), true);
  assert.equal(isKnownContextKey('project.nope'), false);
});

await test('commands.json: the VS Code chords and the named browser fallbacks', () => {
  const chord = (id) => COMMANDS.find((record) => record.id === id)?.keybinding?.default;
  assert.equal(chord('file.newFile'), 'Ctrl+Alt+N');
  assert.equal(chord('file.save'), 'Mod+S');
  assert.equal(chord('view.palette'), 'Mod+Shift+P');
  assert.deepEqual(COMMANDS.find((record) => record.id === 'view.palette').keybinding.also, ['F1']);
  assert.equal(chord('view.quickOpen'), 'Mod+P');
  assert.equal(chord('file.openFolder'), 'Mod+K Mod+O');
  assert.equal(chord('edit.format'), 'Shift+Alt+F');
});

await test('reserved chords are rejected at registration with the named fallback', () => {
  for (const chord of ['Mod+N', 'Mod+T', 'Mod+W', 'Mod+Shift+N', 'Mod+Shift+T', 'Ctrl+Tab', 'Ctrl+PageDown', 'Ctrl+PageUp', 'Mod+1', 'Mod+9', 'Alt+Left', 'Alt+Right']) {
    assert.notEqual(chordProblem(chord), null, chord);
    const problems = validateRecords([{ id: 'x.y', title: 'X', category: 'X', status: 'live', keybinding: { default: chord } }]);
    assert.equal(problems.length, 1, chord);
  }
  assert.match(chordProblem('Mod+N'), /Ctrl\+Alt\+N/);
  assert.match(chordProblem('Mod+W'), /Alt\+W/);
  for (const chord of ['Ctrl+Alt+N', 'Alt+W', 'Mod+S', 'Mod+Shift+P', 'F1', 'Mod+K Mod+O', 'Mod+Shift+F5']) {
    assert.equal(chordProblem(chord), null, chord);
  }
  assert.notEqual(chordProblem('Mod+'), null);
});

await test('ids: unique, well formed, append-only against the committed list', () => {
  const ids = COMMANDS.map((record) => record.id);
  assert.equal(new Set(ids).size, ids.length);
  const committed = json('command-ids.json');
  for (const id of committed) {
    assert.ok(ids.includes(id), `committed id '${id}' was deleted or renamed; retire it with status "removed"`);
  }
  const added = ids.filter((id) => !committed.includes(id));
  assert.deepEqual(added, [], `new ids must be appended to command-ids.json: ${added.join(', ')}`);
  assert.deepEqual(ids.slice(0, committed.length), committed, 'existing ids must keep their order; append new ones at the end');
  const dup = validateRecords([...COMMANDS.slice(0, 2), COMMANDS[0]]);
  assert.ok(dup.some((problem) => problem.includes('duplicate')));
  const bad = validateRecords([{ id: 'NoDot', title: 'x', category: 'x', status: 'live' }]);
  assert.equal(bad.length, 1);
});

await test('ids: a retired id stays as removed and names its replacement', () => {
  const records = [
    { id: 'a.one', title: 'One', category: 'A', status: 'live' },
    { id: 'a.old', title: 'Old', category: 'A', status: 'removed', replacedBy: 'a.one' },
  ];
  assert.deepEqual(validateRecords(records), []);
  assert.equal(validateRecords([{ id: 'a.old', title: 'Old', category: 'A', status: 'removed' }]).length, 1);
  assert.equal(validateRecords([{ id: 'a.old', title: 'Old', category: 'A', status: 'removed', replacedBy: 'a.zzz' }]).length, 1);
});

await test('integrity: live without handler, handler without record, reserved with handler all fail', () => {
  const live = COMMANDS.filter((record) => record.status === 'live').map((record) => record.id);
  assert.deepEqual(checkIntegrity(COMMANDS, live), []);
  assert.match(checkIntegrity(COMMANDS, live.slice(1))[0], /live command .* has no handler/);
  assert.match(checkIntegrity(COMMANDS, [...live, 'nope.missing'])[0], /has no command record/);
  assert.match(checkIntegrity(COMMANDS, [...live, 'go.symbol'])[0], /reserved command 'go.symbol' has a handler/);
  const registry = createRegistry(COMMANDS, { context: createContext() });
  assert.throws(() => registry.register('nope.missing', () => {}), /no command record/);
  assert.throws(() => registry.register('go.symbol', () => {}), /reserved/);
  assert.throws(() => registry.register('file.share', () => {}), /removed/, 'a retired id cannot get a handler back');
  registry.register('file.save', () => {});
  assert.throws(() => registry.register('file.save', () => {}), /already/);
  const retired = COMMANDS.find((record) => record.id === 'file.share');
  assert.equal(retired.status, 'removed');
  assert.equal(retired.replacedBy, null);
  assert.ok(json('command-ids.json').includes('file.share'), 'a retired id stays in the append-only list');
});

await test('run: result codes, disabled with a reason, argument checks, throwing handlers isolated', async () => {
  const logged = [];
  const context = createContext({ 'project.busy': false, 'project.source': 'sample', 'run.enabled': true });
  const registry = createRegistry(COMMANDS, { context, log: (level, event, detail) => logged.push([level, event, detail]) });
  const calls = [];
  registry.register('file.save', () => {
    calls.push('save');
  });
  registry.register('run.restart', () => {
    throw new Error('boom');
  });
  registry.register('device.setViewport', (args) => {
    calls.push(args.value);
  });
  assert.deepEqual(await registry.run('no.such'), { ok: false, code: 'unknown', message: "no command 'no.such'" });
  assert.equal((await registry.run('go.symbol')).code, 'reserved');
  assert.equal((await registry.run('file.newFile')).code, 'no-handler');
  const disabled = await registry.run('file.save');
  assert.equal(disabled.code, 'disabled', 'a sample project cannot Save');
  context.set({ 'project.source': 'folder' });
  assert.deepEqual(await registry.run('file.save'), { ok: true });
  assert.deepEqual(calls, ['save']);
  const failed = await registry.run('run.restart');
  assert.equal(failed.ok, false);
  assert.equal(failed.code, 'failed');
  assert.ok(logged.some(([level, event]) => level === 'error' && event === 'command.failed'));
  assert.equal((await registry.run('device.setViewport')).code, 'invalid-args');
  assert.equal((await registry.run('device.setViewport', { value: 5 })).code, 'invalid-args');
  assert.deepEqual(await registry.run('device.setViewport', { value: '390x844' }), { ok: true });
  const state = registry.describe('file.save');
  assert.equal(state.runnable, true);
  context.set({ 'project.busy': true });
  assert.equal(registry.describe('file.save').runnable, false);
  assert.match(registry.describe('file.save').reason, /unavailable/);
  assert.equal(registry.describe('go.symbol').visible, false);
});

await test('run: a worker service that reports not ready disables its commands with a reason', async () => {
  const context = createContext({ 'run.enabled': true, 'service.viewer.ready': false });
  const registry = createRegistry(COMMANDS, { context });
  registry.register('run.restart', () => {});
  const state = registry.describe('run.restart');
  assert.equal(state.runnable, false);
  assert.match(state.reason, /viewer service is not ready/);
  context.set({ 'service.viewer.ready': true });
  assert.equal(registry.describe('run.restart').runnable, true);
});

await test('checked: a radio item derives from arg.* against a context key', () => {
  const context = createContext({ 'device.viewport': '390x844', 'ui.theme': 'dark' });
  const registry = createRegistry(COMMANDS, { context });
  registry.register('device.setViewport', () => {});
  registry.register('view.setTheme', () => {});
  assert.equal(registry.describe('device.setViewport', { value: '390x844' }).checked, true);
  assert.equal(registry.describe('device.setViewport', { value: '320x240' }).checked, false);
  assert.equal(registry.describe('view.setTheme', { value: 'dark' }).checked, true);
  assert.equal(registry.describe('file.save').checked, null);
});

function keymapFor(commands, overrides = [], platform = 'win') {
  return createKeymap({ commands, overrides, platform });
}
const rec = (id, key, when, extra = {}) => ({ id, title: id, category: 'T', status: 'live', keybinding: { default: key, when }, ...extra });

await test('keys: parse, match and format chords per platform', () => {
  assert.deepEqual(parseChord('Mod+Shift+P', 'win')[0], { ctrl: true, meta: false, alt: false, shift: true, key: 'P' });
  assert.deepEqual(parseChord('Mod+Shift+P', 'mac')[0], { ctrl: false, meta: true, alt: false, shift: true, key: 'P' });
  assert.equal(parseChord('Mod+K Mod+O').length, 2);
  assert.equal(formatChord(parseChord('Mod+Shift+P')), 'Ctrl+Shift+P');
  assert.equal(formatChord(parseChord('Mod+Shift+F5')), 'Ctrl+Shift+F5');
  assert.equal(formatChord(parseChord('Ctrl+Alt+N')), 'Ctrl+Alt+N');
  assert.equal(parseChord('Mod++')[0].key, '+');
  assert.equal(parseChord('Mod+'), null);
  const map = keymapFor([rec('a.one', 'Mod+Shift+P'), rec('a.two', 'F1')]);
  assert.equal(map.resolve(ev('p', { ctrlKey: true, shiftKey: true }), [], {}).id, 'a.one');
  assert.equal(map.resolve(ev('P', { ctrlKey: true, shiftKey: true }), [], {}).id, 'a.one');
  assert.equal(map.resolve(ev('F1'), [], {}).id, 'a.two');
  assert.equal(map.resolve(ev('p', { ctrlKey: true }), [], {}).status, 'none');
  assert.equal(map.resolve(ev('Control', { ctrlKey: true }), [], {}).ignored, true);
  const mac = keymapFor([rec('a.one', 'Mod+Shift+P')], [], 'mac');
  assert.equal(mac.resolve(ev('p', { metaKey: true, shiftKey: true }), [], {}).id, 'a.one');
  assert.equal(mac.resolve(ev('p', { ctrlKey: true, shiftKey: true }), [], {}).status, 'none');
});

await test('keys: chords wait for the second step, and fall back to the key alone', () => {
  const map = keymapFor([rec('a.open', 'Mod+K Mod+O'), rec('a.zen', 'Mod+K Z'), rec('a.plain', 'O')]);
  const first = map.resolve(ev('k', { ctrlKey: true }), [], {});
  assert.equal(first.status, 'pending');
  const second = map.resolve(ev('o', { ctrlKey: true }), first.pending, {});
  assert.deepEqual([second.status, second.id], ['match', 'a.open']);
  const zen = map.resolve(ev('z'), first.pending, {});
  assert.equal(zen.id, 'a.zen');
  const fallback = map.resolve(ev('o'), first.pending, {});
  assert.equal(fallback.id, 'a.plain', 'a failed chord retries the key as a fresh chord');
  const none = map.resolve(ev('q'), first.pending, {});
  assert.equal(none.status, 'none');
});

await test('keys: when scopes a binding and the most specific when wins', () => {
  const map = keymapFor([
    rec('a.generic', 'F2', 'tree.focus'),
    rec('a.specific', 'F2', 'tree.focus && project.open'),
    rec('a.editor', 'F2', 'editor.focus'),
    rec('a.always', 'Mod+S'),
  ]);
  assert.equal(map.resolve(ev('F2'), [], { 'tree.focus': true }).id, 'a.generic');
  assert.equal(map.resolve(ev('F2'), [], { 'tree.focus': true, 'project.open': true }).id, 'a.specific');
  assert.equal(map.resolve(ev('F2'), [], { 'editor.focus': true }).id, 'a.editor');
  assert.equal(map.resolve(ev('F2'), [], {}).status, 'none');
  assert.equal(map.resolve(ev('s', { ctrlKey: true }), [], {}).id, 'a.always');
});

await test('keys: a user override beats a default, removal unbinds, a reserved override is dropped', () => {
  const commands = [rec('a.one', 'Mod+S'), rec('a.two', 'Mod+Shift+S')];
  const map = keymapFor(commands, [
    { key: 'Mod+S', command: 'a.two' },
    { key: 'Mod+Shift+S', command: '-a.two' },
    { key: 'Mod+W', command: 'a.one' },
  ]);
  assert.equal(map.resolve(ev('s', { ctrlKey: true }), [], {}).id, 'a.two');
  assert.equal(map.resolve(ev('s', { ctrlKey: true, shiftKey: true }), [], {}).status, 'none');
  assert.equal(map.resolve(ev('w', { ctrlKey: true }), [], {}).status, 'none');
  assert.deepEqual(map.rejected.map((entry) => entry.reason), ['reserved']);
  assert.equal(map.chordFor('a.two'), 'Ctrl+S');
  assert.equal(map.chordFor('a.one'), 'Ctrl+S');
  assert.ok(map.conflicts().some((conflict) => conflict.chord === 'Ctrl+S'));
});

await test('keys: reserved and removed commands bind nothing, the registry wires handlers in', () => {
  const registry = createRegistry(COMMANDS, { context: createContext({ 'tree.focus': true }) });
  assert.equal(registry.keymap().resolve(ev('F12'), [], {}).status, 'none');
  assert.equal(registry.keymap().resolve(ev('s', { ctrlKey: true }), [], {}).status, 'none', 'no handler, no binding');
  registry.register('file.save', () => {});
  assert.equal(registry.keymap().resolve(ev('s', { ctrlKey: true }), [], {}).id, 'file.save');
  assert.equal(registry.keymap().chordFor('file.save'), 'Ctrl+S');
  registry.register('edit.redo', () => {});
  assert.equal(registry.keymap().resolve(ev('z', { ctrlKey: true, shiftKey: true }), [], { 'editor.focus': true }).id, 'edit.redo', 'an also chord binds too');
  assert.equal(registry.keymap().chordFor('edit.redo'), 'Ctrl+Y');
});

await test('workbench-core imports no react, codemirror, dock library or DOM global', () => {
  const banned = [/from\s+['"]react/, /from\s+['"]react-dom/, /from\s+['"]@codemirror\//, /from\s+['"]flexlayout/, /from\s+['"]dockview/, /\b(document|window|navigator|localStorage|HTMLElement)\b\./, /\bnew (Worker|MutationObserver)\b/];
  for (const name of readdirSync(WORKBENCH).filter((file) => file.endsWith('.js'))) {
    const code = readFileSync(resolve(WORKBENCH, name), 'utf8').replace(/\/\/.*$/gm, '');
    for (const pattern of banned) {
      assert.equal(pattern.test(code), false, `${name} matches ${pattern}`);
    }
  }
});

finish('preview_workbench');
