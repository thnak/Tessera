// SPDX-License-Identifier: MIT
//
// The command palette's acceptance check (issue #558; `docs/specs/studio-workbench-v1.md` 2.1, 3.1, 6).
//
//   node scripts/ci/preview_workbench_palette.mjs
//
// Pure Node: `palette.js` is a plain module. Proves the fuzzy ranking (word starts and consecutive runs beat
// scattered matches, a basename can win a path), the recent-commands list (capped, deduplicated, ranks an
// empty query and breaks ties), the one-input modes (`>` commands, `:` line, `name:12` file at line), go to
// line parsing, the commands the palette offers, and the argument prompt flow built from a command's JSON
// Schema (choices from `enum` and from providers, typed numbers with bounds, `back`).

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { assert, finish, test } from './mock_test_support.mjs';

const WORKBENCH = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'designer', 'web', 'src', 'workbench');
const load = (name) => import(pathToFileURL(resolve(WORKBENCH, name)).href);
const COMMANDS = JSON.parse(readFileSync(resolve(WORKBENCH, 'commands.json'), 'utf8')).commands;

const { fuzzyMatch, rankItems, createRecentList, commandItems, parseInput, parseLine, createArgFlow, needsPrompt } = await load('palette.js');
const { createContext } = await load('context.js');
const { createRegistry } = await load('registry.js');

const top = (query, labels, options) => rankItems(query, labels.map((label) => ({ id: label, label })), options).map((entry) => entry.item.label);

await test('fuzzy: a subsequence matches with its positions, anything else is null', () => {
  assert.deepEqual(fuzzyMatch('ap', 'App.tsx').positions, [0, 1]);
  assert.equal(fuzzyMatch('xa', 'App.tsx'), null);
  assert.equal(fuzzyMatch('zzzz', 'zz'), null);
  assert.deepEqual(fuzzyMatch('', 'anything'), { score: 0, positions: [] });
  assert.deepEqual(fuzzyMatch('ST', 'File: Save Test').positions.length, 2);
  assert.ok(fuzzyMatch('APP', 'app.tsx') !== null, 'case-insensitive');
});

await test('fuzzy: word starts, camel humps and consecutive runs beat scattered matches', () => {
  assert.equal(top('save', ['File: Download Project (.zip)', 'Device: Skin Variants', 'File: Save'])[0], 'File: Save');
  assert.equal(top('ss', ['Device: Skin', 'View: Toggle Status Bar', 'File: Save Something'])[0], 'File: Save Something');
  assert.equal(top('tb', ['ui/ThemeButton.tsx', 'ui/atbq.tsx'])[0], 'ui/ThemeButton.tsx');
  assert.ok(fuzzyMatch('sh', 'Show Shortcuts').score > fuzzyMatch('sh', 'Download Project (.zip) Shortcuts').score - 5);
  assert.deepEqual(top('zzz', ['a', 'b']), [], 'no match is dropped');
});

await test('rank: a file name can win over the path, and the path still matches', () => {
  const files = ['ui/components/ThemeHomeButton.tsx', 'styles/homepage-extra.css', 'ui/Home.tsx'].map((path) => ({ id: path, label: path, basename: path.split('/').at(-1) }));
  const ranked = rankItems('home', files);
  assert.equal(ranked[0].item.label, 'ui/Home.tsx');
  assert.equal(ranked[0].basenameMatch, true);
  assert.deepEqual(ranked[0].positions, [3, 4, 5, 6], 'positions are indices into the whole label');
  assert.ok(rankItems('ui/hom', files).some((entry) => entry.item.label === 'ui/Home.tsx'), 'a path query still matches');
});

await test('recent: capped, deduplicated, most recent first, loads defensively', () => {
  const recent = createRecentList(3);
  for (const id of ['a', 'b', 'c', 'a', 'd']) {
    recent.touch(id);
  }
  assert.deepEqual(recent.list(), ['d', 'a', 'c']);
  assert.equal(recent.indexOf('a'), 1);
  assert.equal(recent.indexOf('zzz'), -1);
  recent.load(['x', 'x', 7, 'y', 'z', 'w']);
  assert.deepEqual(recent.list(), ['x', 'y', 'z']);
  recent.load('garbage');
  assert.deepEqual(recent.list(), []);
  assert.deepEqual(JSON.parse(JSON.stringify(createRecentList())), []);
});

await test('rank: an empty query lists recent first then alphabetical; recency breaks a score tie', () => {
  const recent = createRecentList();
  recent.touch('File: Save');
  recent.touch('Run: Restart App');
  assert.deepEqual(top('', ['View: Zen', 'File: Save', 'Edit: Undo', 'Run: Restart App'], { recent }), ['Run: Restart App', 'File: Save', 'Edit: Undo', 'View: Zen']);
  assert.equal(top('run', ['Run: One', 'Run: Two'], { recent: { indexOf: (id) => (id === 'Run: Two' ? 0 : -1) } })[0], 'Run: Two', 'equal scores: the recent one first');
});

await test('input: the prefix chooses the mode, name:12 is a file at a line', () => {
  assert.deepEqual(parseInput('>save'), { mode: 'commands', text: 'save' });
  assert.deepEqual(parseInput('> save'), { mode: 'commands', text: 'save' });
  assert.deepEqual(parseInput('>'), { mode: 'commands', text: '' });
  assert.deepEqual(parseInput(':42'), { mode: 'line', text: '42' });
  assert.deepEqual(parseInput('App.tsx:42'), { mode: 'files', text: 'App.tsx', line: 42 });
  assert.deepEqual(parseInput('App.tsx'), { mode: 'files', text: 'App.tsx' });
  assert.deepEqual(parseInput(''), { mode: 'files', text: '' });
});

await test('go to line: numbers clamp to the document, anything else says why', () => {
  assert.deepEqual(parseLine('7', 20), { ok: true, line: 7 });
  assert.deepEqual(parseLine('999', 20), { ok: true, line: 20 });
  assert.equal(parseLine('', 20).ok, false);
  assert.match(parseLine('', 20).message, /between 1 and 20/);
  assert.equal(parseLine('abc', 20).ok, false);
  assert.equal(parseLine('0', 20).ok, false);
  assert.equal(parseLine('3', 0).ok, false);
  assert.match(parseLine('3', 0).message, /No file is open/);
});

function bootRegistry(values = {}) {
  const context = createContext({ 'project.open': true, 'project.busy': false, 'project.source': 'folder', 'project.folderSupported': true, 'run.enabled': true, 'canvas.available': true, ...values });
  const registry = createRegistry(COMMANDS, { context });
  for (const record of COMMANDS) {
    if (record.status === 'live') {
      registry.register(record.id, () => {});
    }
  }
  return registry;
}

await test('commands: only live, runnable, palette-surface commands as Category: Title with their chord', () => {
  const items = commandItems(bootRegistry());
  const byId = Object.fromEntries(items.map((item) => [item.id, item]));
  assert.equal(byId['file.save'].label, 'File: Save');
  assert.equal(byId['file.save'].chord, 'Ctrl+S');
  assert.equal(byId['view.palette'].chord, 'Ctrl+Shift+P');
  assert.ok(byId['view.cycleTheme'], 'a palette-and-toolbar command is offered');
  assert.ok(byId['device.setViewport'], 'a command with arguments is offered (the prompt asks for them)');
  assert.equal(byId['go.definition'], undefined, 'reserved commands are not offered');
  assert.equal(byId['edit.format'].label, 'Edit: Format Document', 'Format Document is live (ADR-061 slice 2)');
  assert.equal(byId['edit.format'].chord, 'Alt+Shift+F');
  assert.equal(commandItems(bootRegistry({ 'project.source': 'sample' })).some((item) => item.id === 'file.save'), false, 'a command that is not runnable now is not offered');
  assert.equal(items.every((item) => /^[A-Z][a-z]+: /.test(item.label)), true);
});

const record = (id) => COMMANDS.find((entry) => entry.id === id);
const providers = {
  viewports: () => [{ label: 'Phone 390x844', args: { value: '390x844' } }, { label: 'Tablet 768x1024', args: { value: '768x1024' } }],
  recent: () => [{ label: 'proj', detail: '11/19/2026', args: { index: 0 } }, { label: 'other', args: { index: 1 } }],
};

await test('args: a choice step lists provider entries with the value the command takes', () => {
  assert.equal(needsPrompt(record('device.setViewport')), true);
  assert.equal(needsPrompt(record('device.setViewport'), { value: '390x844' }), false);
  assert.equal(needsPrompt(record('file.save')), false);
  const flow = createArgFlow(record('device.setViewport'), {}, providers);
  assert.equal(flow.done, false);
  assert.equal(flow.step.kind, 'choice');
  assert.deepEqual(flow.step.choices.map((choice) => [choice.label, choice.value]), [['Phone 390x844', '390x844'], ['Tablet 768x1024', '768x1024']]);
  const done = flow.answer('390x844');
  assert.equal(done.done, true);
  assert.deepEqual(done.args, { value: '390x844' });
  const recent = createArgFlow(record('file.openRecent'), {}, providers);
  assert.deepEqual(recent.step.choices.map((choice) => choice.value), [0, 1]);
});

await test('args: an enum is a choice without a provider; given arguments skip their step', () => {
  const fold = createArgFlow(record('device.setFold'));
  assert.deepEqual(fold.step.choices.map((choice) => choice.value), ['none', 'flat', 'book', 'tabletop']);
  const skipped = createArgFlow(record('device.setCustomSize'), { width: 800 });
  assert.equal(skipped.steps.length, 1);
  assert.equal(skipped.step.name, 'height');
  assert.deepEqual(createArgFlow(record('file.save')).done, true);
});

await test('args: typed numbers are validated against type and bounds, then collected in order', () => {
  let flow = createArgFlow(record('device.setCustomSize'));
  assert.deepEqual(flow.steps.map((step) => [step.name, step.kind]), [['width', 'number'], ['height', 'number']]);
  for (const [text, expected] of [['', /required/], ['abc', /whole number/], ['1.5', /whole number/], ['50', /120/], ['99999', /4096/]]) {
    const refused = flow.answerText(text);
    assert.equal(refused.ok, false, text);
    assert.match(refused.message, expected, text);
  }
  const first = flow.answerText(' 800 ');
  assert.equal(first.ok, true);
  assert.equal(first.flow.step.name, 'height');
  const second = first.flow.answerText('600');
  assert.equal(second.flow.done, true);
  assert.deepEqual(second.flow.args, { width: 800, height: 600 });
  assert.deepEqual(first.flow.back().args, {});
  assert.equal(first.flow.back().step.name, 'width');
  assert.equal(flow.back().step.name, 'width', 'back at the first step stays there');
  flow = null;
});

await test('args: the finished arguments satisfy the registry (the prompt feeds run())', async () => {
  const registry = bootRegistry();
  const flow = createArgFlow(record('device.setCustomSize')).answerText('800').flow.answerText('600').flow;
  assert.deepEqual(await registry.run('device.setCustomSize', flow.args), { ok: true });
  assert.equal((await registry.run('device.setCustomSize', { width: 5, height: 600 })).code, 'invalid-args');
});

finish('preview_workbench_palette');
