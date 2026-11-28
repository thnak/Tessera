// SPDX-License-Identifier: MIT
//
// The page side of project search (#580): the client around the search worker (a pattern error is a sentence,
// a superseded search is null, a killed worker is restarted from the next snapshot) and the replace model
// (edits applied as one operation, the multi-file undo and redo with the files edited since left alone). The
// modules are imported from dist-ssr (the code the page ships; \`npm run build:chrome\` is a prerequisite and a
// missing build is exit code 2).
//
//   node scripts/ci/preview_search_client.mjs

import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { Worker } from 'node:worker_threads';

import { searchServiceSource } from '../../designer/web/services/search-service.js';
import { assert, finish, test } from './mock_test_support.mjs';

const DIST = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'designer', 'web', 'dist-ssr');
let client;
let model;
try {
  client = await import(pathToFileURL(resolve(DIST, 'search-client.js')).href);
  model = await import(pathToFileURL(resolve(DIST, 'search-replace.js')).href);
} catch {
  process.stderr.write('preview_search_client: ' + DIST + ' is missing; run npm run build:chrome --prefix designer/web first\n');
  process.exit(2);
}

function nodeServiceWorker(source) {
  const shim = [
    'const { parentPort } = require("node:worker_threads");',
    'globalThis.postMessage = (m) => parentPort.postMessage(m);',
    'globalThis.addEventListener = (t, f) => parentPort.on("message", (d) => f({ data: d }));',
  ].join('\n');
  const worker = new Worker(shim + '\n' + source, { eval: true });
  return {
    post: (m, transfer = []) => worker.postMessage(m, transfer),
    onMessage: (f) => worker.on('message', f),
    onError: (f) => worker.on('error', (e) => f(String((e && e.message) || e))),
    terminate: () => void worker.terminate(),
  };
}

const QUERY = { query: 'card', caseSensitive: false, wholeWord: false, regex: false, include: '', exclude: '' };
const ENTRIES = [
  [0, 'Card card\n', 'ui/a.tsx'],
  [1, 'a card\nno\ncard card\n', 'ui/b.tsx'],
];
const make = () => client.createSearchClient(() => nodeServiceWorker(searchServiceSource()));

await test('client: a search answers the files and spans, paths included', async () => {
  const c = make();
  const outcome = await c.search(ENTRIES, QUERY);
  assert.equal(outcome.ok, true);
  assert.deepEqual(outcome.value.files.map((g) => [g.path, g.matches.length]), [['ui/a.tsx', 2], ['ui/b.tsx', 3]]);
  c.dispose();
});

await test('client: a bad pattern is a sentence, not a throw', async () => {
  const c = make();
  const outcome = await c.search(ENTRIES, { ...QUERY, query: '(?=a)', regex: true });
  assert.equal(outcome.ok, false);
  assert.match(outcome.message, /lookaround/);
  c.dispose();
});

await test('client: a newer search supersedes an older one (null, never a stale list)', async () => {
  const c = make();
  const big = Array.from({ length: 200 }, (_, i) => [i, 'needle needle\n'.repeat(3000), 'f' + i + '.txt']);
  const first = c.search(big, { ...QUERY, query: 'needle', regex: true }, 50000);
  const second = c.search(big, { ...QUERY, query: 'needle needle' }, 3);
  assert.equal(await first, null);
  assert.equal((await second).value.total, 3);
  c.dispose();
});

await test('client: a replace plan carries edits the model applies', async () => {
  const c = make();
  const outcome = await c.replace(ENTRIES, QUERY, 'Tile', null);
  assert.equal(outcome.ok, true);
  assert.equal(outcome.value.edits.length, 5);
  const texts = ENTRIES.map(([id, text]) => model.applyOriginalEdits(text, outcome.value.edits.filter((e) => e.file === id)));
  assert.deepEqual(texts, ['Tile Tile\n', 'a Tile\nno\nTile Tile\n']);
  c.dispose();
});

await test('client: a killed worker is restarted and the request still answers', async () => {
  let started = 0;
  let current;
  let report;
  const c = client.createSearchClient(() => {
    started += 1;
    current = nodeServiceWorker(searchServiceSource());
    return { ...current, onError: (f) => { report = f; current.onError(f); } };
  });
  assert.equal((await c.search(ENTRIES, QUERY)).ok, true);
  current.terminate();
  report('killed');
  const outcome = await c.search(ENTRIES, QUERY);
  assert.equal(outcome.ok, true);
  assert.equal(started, 2);
  c.dispose();
});

await test('apply: overlapping or out-of-range edits are refused, order does not matter', () => {
  assert.equal(model.applyOriginalEdits('abcdef', [{ offset: 4, length: 1, newText: 'X' }, { offset: 0, length: 2, newText: '' }]), 'cdXf');
  assert.throws(() => model.applyOriginalEdits('abc', [{ offset: 0, length: 2, newText: '' }, { offset: 1, length: 1, newText: '' }]), RangeError);
  assert.throws(() => model.applyOriginalEdits('abc', [{ offset: 2, length: 5, newText: '' }]), RangeError);
});

const entry = (label, ...changes) => ({ label, changes: changes.map(([path, before, after]) => ({ path, before, after })) });
const store = (init) => {
  const texts = new Map(Object.entries(init));
  return { texts, textOf: (path) => texts.get(path) ?? '', write: (writes) => writes.forEach((w) => texts.set(w.path, w.text)) };
};

await test('history: one undo restores every file of a multi-file replace, redo replays it', () => {
  const history = model.createReplaceHistory();
  const s = store({ 'a.tsx': 'Y', 'b.tsx': 'Y' });
  history.push(entry('Replace x with Y', ['a.tsx', 'x', 'Y'], ['b.tsx', 'x', 'Y']));
  assert.equal(history.canUndo(), true);
  assert.equal(history.undoLabel(), 'Replace x with Y');
  const undone = history.undo(s.textOf);
  s.write(undone.writes);
  assert.deepEqual([...s.texts], [['a.tsx', 'x'], ['b.tsx', 'x']]);
  assert.equal(history.canUndo(), false);
  assert.equal(history.canRedo(), true);
  s.write(history.redo(s.textOf).writes);
  assert.deepEqual([...s.texts], [['a.tsx', 'Y'], ['b.tsx', 'Y']]);
});

await test('history: a file edited since is left alone and reported, the rest still undoes', () => {
  const history = model.createReplaceHistory();
  const s = store({ 'a.tsx': 'Y plus typing', 'b.tsx': 'Y' });
  history.push(entry('r', ['a.tsx', 'x', 'Y'], ['b.tsx', 'x', 'Y']));
  const undone = history.undo(s.textOf);
  assert.deepEqual(undone.skipped, ['a.tsx']);
  assert.deepEqual(undone.writes, [{ path: 'b.tsx', text: 'x' }]);
});

await test('history: a new replace clears redo; the stack is bounded; an empty stack answers null', () => {
  const history = model.createReplaceHistory();
  const s = store({ 'a.tsx': 'Y' });
  assert.equal(history.undo(s.textOf), null);
  history.push(entry('one', ['a.tsx', 'x', 'Y']));
  history.undo(s.textOf);
  assert.equal(history.canRedo(), true);
  history.push(entry('two', ['a.tsx', 'x', 'Y']));
  assert.equal(history.canRedo(), false);
  for (let i = 0; i < model.MAX_REPLACE_HISTORY + 5; i += 1) history.push(entry('n' + i, ['a.tsx', 'x', 'Y']));
  let steps = 0;
  while (history.canUndo()) { history.undo(() => 'Y'); steps += 1; }
  assert.equal(steps, model.MAX_REPLACE_HISTORY);
});

await test('label: names the strings and the counts, long strings shortened', () => {
  assert.equal(model.replaceLabel('a', 'b', 1, 1), 'Replace "a" with "b" (1 in 1 file)');
  assert.equal(model.replaceLabel('a', 'b', 4, 2), 'Replace "a" with "b" (4 in 2 files)');
  assert.match(model.replaceLabel('x'.repeat(60), 'y', 2, 2), /^Replace "x{23}…" with "y"/);
});

finish('preview_search_client');
