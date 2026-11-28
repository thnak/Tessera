// SPDX-License-Identifier: MIT
//
// The version history panel's model (#583): the timeline rows (reason, age, distance from the working text),
// the bounded changed-line view, and the restore plan -- one minimal edit that yields the snapshot's bytes, with
// the replaced working text kept as a 'restore' version. Also the rename moves of `snapshots.js` (a file, a
// folder) and an end-to-end record, rename and restore over the store. The model is imported from dist-ssr
// (`npm run build:chrome` is a prerequisite; a missing build is exit code 2).
//
//   node scripts/ci/preview_history_model.mjs

import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { addSnapshot, applyEdit, emptyHistory, renamePath, renameTree } from '../../designer/web/snapshots.js';
import { createSnapshotStore } from '../../designer/web/snapshot-store.js';
import { assert, finish, test } from './mock_test_support.mjs';

const DIST = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'designer', 'web', 'dist-ssr');
let model;
try {
  model = await import(pathToFileURL(resolve(DIST, 'history-model.js')).href);
} catch {
  process.stderr.write('preview_history_model: ' + DIST + ' is missing; run npm run build:chrome --prefix designer/web first\n');
  process.exit(2);
}

const record = (history, path, text, reason, at) => addSnapshot(history, { path, text, reason, at }).history;

await test('age labels are deterministic and coarse', () => {
  const now = 10_000_000;
  assert(model.formatAge(now - 1000, now) === 'just now');
  assert(model.formatAge(now - 5 * 60_000, now) === '5 min ago');
  assert(model.formatAge(now - 3 * 3_600_000, now) === '3 h ago');
  assert(/^\d{4}-\d\d-\d\d \d\d:\d\d UTC$/.test(model.formatAge(now - 3 * 86_400_000, now)));
  assert(model.formatAge(now + 5000, now) === 'just now', 'a clock behind the snapshot is not negative');
  assert(model.reasonLabel('format') === 'Before format' && model.reasonLabel('future') === 'future');
});

await test('the timeline is newest first and compared with the working text', () => {
  let h = emptyHistory();
  h = record(h, 'a.tsx', 'one\ntwo\n', 'save', 1000);
  h = record(h, 'b.tsx', 'other\n', 'save', 1500);
  h = record(h, 'a.tsx', 'one\ntwo\nthree\n', 'format', 2000);
  const rows = model.describeTimeline(h, 'a.tsx', 'one\ntwo\nthree\n', 2000);
  assert(rows.length === 2 && rows[0].label === 'Before format' && rows[0].current === true, JSON.stringify(rows));
  assert(rows[1].label === 'Saved' && rows[1].added === 1 && rows[1].removed === 0, JSON.stringify(rows[1]));
  assert(model.describeTimeline(h, 'missing.tsx', '', 0).length === 0);
});

await test('the changed-line view numbers lines and is capped with a count of the rest', () => {
  const view = model.diffRows('a\nb\nc\n', 'a\nB\nc\nd\n');
  assert(JSON.stringify(view.rows.map((r) => [r.kind, r.line, r.text])) === JSON.stringify([['delete', 2, 'b'], ['insert', 2, 'B'], ['insert', 4, 'd']]), JSON.stringify(view));
  assert(view.hidden === 0);
  const big = Array.from({ length: model.MAX_DIFF_LINES + 50 }, (_, i) => 'l' + i + '\n').join('');
  const capped = model.diffRows('', big);
  assert(capped.rows.length === model.MAX_DIFF_LINES && capped.hidden === 50, capped.rows.length + ' ' + capped.hidden);
});

await test('restore is one minimal edit that yields the snapshot and keeps the working text', () => {
  const working = 'const a = 1;\nconst b = 2;\n';
  const old = 'const a = 1;\nconst b = 3;\nconst c = 4;\n';
  const plan = model.planRestore(working, old);
  assert(!plan.noop && plan.keep.reason === 'restore' && plan.keep.text === working);
  assert(applyEdit(working, plan.edit) === old, 'the edit gives back the snapshot exactly');
  assert(plan.edit.from > 0, 'the unchanged prefix is untouched');
  const same = model.planRestore(old, old);
  assert(same.noop && same.keep === null);
  const emoji = model.planRestore('a\u{1F600}b', 'a\u{1F601}b');
  assert(applyEdit('a\u{1F600}b', emoji.edit) === 'a\u{1F601}b' && emoji.edit.to - emoji.edit.from === 2, 'a surrogate pair is replaced whole');
});

await test('Restore Previous Version picks the newest snapshot that differs', () => {
  let h = emptyHistory();
  h = record(h, 'a.tsx', 'v1', 'save', 1);
  h = record(h, 'a.tsx', 'v2', 'save', 2);
  assert(model.previousVersion(h, 'a.tsx', 'v2').text === 'v1');
  assert(model.previousVersion(h, 'a.tsx', 'v3').text === 'v2');
  assert(model.previousVersion(h, 'a.tsx', 'v1').text === 'v2');
  assert(model.previousVersion(emptyHistory(), 'a.tsx', 'x') === undefined);
});

await test('a rename moves a file or a folder with its versions and leaves a sibling alone', () => {
  let h = emptyHistory();
  h = record(h, 'ui/a.tsx', 'a', 'save', 1);
  h = record(h, 'ui/deep/b.tsx', 'b', 'save', 2);
  h = record(h, 'uikit/c.tsx', 'c', 'save', 3);
  const file = renamePath(h, 'ui/a.tsx', 'ui/z.tsx');
  assert(file.entries.map((e) => e.path).join() === 'ui/z.tsx,ui/deep/b.tsx,uikit/c.tsx');
  const tree = renameTree(h, 'ui', 'views');
  assert(tree.entries.map((e) => e.path).join() === 'views/a.tsx,views/deep/b.tsx,uikit/c.tsx', tree.entries.map((e) => e.path).join());
});

await test('record, rename and restore over the store end to end', async () => {
  const data = new Map();
  const store = createSnapshotStore({ get: async (k) => data.get(k), put: async (k, v) => void data.set(k, structuredClone(v)), delete: async (k) => void data.delete(k) });
  const key = 'folder:demo';
  await store.record(key, { path: 'a.tsx', text: 'one\n', reason: 'save', at: 1 });
  await store.record(key, { path: 'a.tsx', text: 'one\ntwo\n', reason: 'format', at: 2 });
  const moved = renamePath(await store.load(key), 'a.tsx', 'b.tsx');
  assert((await store.save(key, moved)).ok);
  const loaded = await store.load(key);
  const rows = model.describeTimeline(loaded, 'b.tsx', 'one\ntwo\nthree\n', 3);
  assert(rows.length === 2 && rows[0].added === 1 && rows[1].added === 2, JSON.stringify(rows));
  const target = model.previousVersion(loaded, 'b.tsx', 'one\ntwo\nthree\n');
  const plan = model.planRestore('one\ntwo\nthree\n', target.text);
  await store.record(key, { path: 'b.tsx', text: plan.keep.text, reason: plan.keep.reason, at: 4 });
  assert(applyEdit('one\ntwo\nthree\n', plan.edit) === 'one\ntwo\n');
  const after = model.describeTimeline(await store.load(key), 'b.tsx', 'one\ntwo\n', 5);
  assert(after[0].label === 'Before restore' && after.length === 3, JSON.stringify(after));
});

finish('preview_history_model');
