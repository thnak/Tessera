// SPDX-License-Identifier: MIT
//
// Version history for project files, tested without a browser (issue #583).
//
//   node scripts/ci/preview_snapshots.mjs
//
// Covers the pure policy (cap, dedupe, deterministic ids, byte budget, restore yields exact bytes), the
// deterministic line diff with UTF-16 spans, and the store's handling of storage that throws.
import assert from 'node:assert/strict';

import { diffLines, splitLines } from '../../designer/web/line-diff.js';
import { createSnapshotStore } from '../../designer/web/snapshot-store.js';
import {
  SNAPSHOT_LIMITS,
  addSnapshot,
  applyEdit,
  emptyHistory,
  findSnapshot,
  forgetPath,
  renamePath,
  restoreEdit,
  sanitizeHistory,
  timeline,
  utf8Bytes,
} from '../../designer/web/snapshots.js';

let failures = 0;
async function test(name, body) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

const add = (history, path, text, at = 1, limits) => addSnapshot(history, { path, text, reason: 'save', at }, limits);

await test('a save is recorded, newest first in the timeline', () => {
  let h = add(emptyHistory(), 'a.tsx', 'one').history;
  h = add(h, 'a.tsx', 'two', 2).history;
  assert.deepEqual(timeline(h, 'a.tsx').map((e) => e.text), ['two', 'one']);
});

await test('identical consecutive saves are one snapshot; an older text can come back', () => {
  let r = add(emptyHistory(), 'a.tsx', 'one');
  r = add(r.history, 'a.tsx', 'one');
  assert.equal(r.added, false);
  assert.equal(r.skipped, 'duplicate');
  r = add(r.history, 'a.tsx', 'two');
  r = add(r.history, 'a.tsx', 'one');
  assert.equal(r.added, true);
  assert.equal(r.history.entries.length, 3);
});

await test('another file with the same text is not a duplicate', () => {
  const h = add(emptyHistory(), 'a.tsx', 'x').history;
  assert.equal(add(h, 'b.tsx', 'x').added, true);
});

await test('the per-file cap drops the oldest of that file only', () => {
  const limits = { ...SNAPSHOT_LIMITS, maxPerFile: 3 };
  let h = add(emptyHistory(), 'other.css', 'keep', 1, limits).history;
  for (let i = 0; i < 6; i += 1) {
    h = add(h, 'a.tsx', `v${i}`, i, limits).history;
  }
  assert.deepEqual(timeline(h, 'a.tsx').map((e) => e.text), ['v5', 'v4', 'v3']);
  assert.equal(timeline(h, 'other.css').length, 1);
});

await test('the byte budget evicts the oldest overall and keeps the newest', () => {
  const limits = { maxPerFile: 50, maxFileBytes: 10, maxTotalBytes: 25 };
  let h = emptyHistory();
  for (const text of ['aaaaaaaaaa', 'bbbbbbbbbb', 'cccccccccc']) {
    h = add(h, 'a.tsx', text, 1, limits).history;
  }
  assert.deepEqual(h.entries.map((e) => e.text), ['bbbbbbbbbb', 'cccccccccc']);
});

await test('a file over the size cap is refused with a reason, not truncated', () => {
  const r = add(emptyHistory(), 'a.tsx', 'x'.repeat(SNAPSHOT_LIMITS.maxFileBytes + 1));
  assert.equal(r.added, false);
  assert.equal(r.skipped, 'too-large');
});

await test('ids are deterministic and the history is not mutated', () => {
  const base = emptyHistory();
  const one = add(base, 'a.tsx', 'hello').history;
  const two = add(base, 'a.tsx', 'hello').history;
  assert.equal(one.entries[0].id, two.entries[0].id);
  assert.equal(base.entries.length, 0);
  assert.equal(findSnapshot(one, one.entries[0].id).text, 'hello');
  assert.notEqual(add(one, 'a.tsx', 'hello2').history.entries[1].id, one.entries[0].id);
});

await test('utf8Bytes matches TextEncoder, surrogate pairs included', () => {
  for (const text of ['', 'abc', 'é€', '😀 x', '\ud800 lone']) {
    assert.equal(utf8Bytes(text), new TextEncoder().encode(text).length);
  }
});

await test('rename moves the versions, forget drops them', () => {
  const h = add(emptyHistory(), 'a.tsx', 'x').history;
  assert.equal(timeline(renamePath(h, 'a.tsx', 'b.tsx'), 'b.tsx').length, 1);
  assert.equal(forgetPath(h, 'a.tsx').entries.length, 0);
});

await test('a corrupt stored value is an empty history', () => {
  for (const bad of [null, 3, { version: 2 }, { version: 1, entries: 'x', next: 1 }]) {
    assert.deepEqual(sanitizeHistory(bad), emptyHistory());
  }
  const mixed = sanitizeHistory({ version: 1, next: 5, entries: [{ id: 'a', path: 'p', text: 't' }, { nope: 1 }] });
  assert.equal(mixed.entries.length, 1);
});

await test('restore produces the exact bytes for many shapes, one minimal replacement', () => {
  const cases = [
    ['', ''], ['', 'abc'], ['abc', ''], ['abc', 'abc'], ['hello world', 'hello brave world'],
    ['a\r\nb\n', 'a\nb'], ['x😀y', 'x😁y'], ['😀', '😁'], ['aaa', 'aa'], ['aa', 'aaa'], ['é€\u0000z', '€é'],
  ];
  for (const [current, target] of cases) {
    const edit = restoreEdit(current, target);
    assert.equal(applyEdit(current, edit), target, JSON.stringify([current, target]));
    assert.ok(edit.from <= edit.to && edit.to <= current.length);
  }
  assert.deepEqual(restoreEdit('hello world', 'hello brave world'), { from: 6, to: 6, insert: 'brave ' });
  assert.deepEqual(restoreEdit('same', 'same'), { from: 4, to: 4, insert: '' });
});

// ---- line diff ----

const rebuild = (segments, side) =>
  segments
    .filter((s) => s.kind === 'equal' || s.kind === (side === 'a' ? 'delete' : 'insert'))
    .map((s) => s.lines.join(''))
    .join('');

await test('diff segments rebuild both texts and carry UTF-16 spans', () => {
  const a = 'one\ntwo\nthree\n';
  const b = 'one\n2\nthree\nfour';
  const segments = diffLines(a, b);
  assert.equal(rebuild(segments, 'a'), a);
  assert.equal(rebuild(segments, 'b'), b);
  assert.deepEqual(segments.map((s) => s.kind), ['equal', 'delete', 'insert', 'equal', 'insert']);
  const del = segments[1];
  assert.equal(a.slice(del.aFrom, del.aTo), 'two\n');
  const ins = segments[4];
  assert.equal(b.slice(ins.bFrom, ins.bTo), 'four');
  assert.equal(del.bFrom, del.bTo);
});

await test('diff spans count UTF-16 units, not code points', () => {
  const a = '😀\nx\n';
  const b = '😀\ny\n';
  const del = diffLines(a, b).find((s) => s.kind === 'delete');
  assert.equal(del.aFrom, 3);
  assert.equal(del.aTo, 5);
});

await test('diff is minimal on a known case and deterministic', () => {
  const a = 'a\nb\nc\na\nb\nb\na\n';
  const b = 'c\nb\na\nb\na\nc\n';
  const segments = diffLines(a, b);
  const edits = segments.filter((s) => s.kind !== 'equal').reduce((n, s) => n + s.lines.length, 0);
  assert.equal(edits, 5); // Myers' classic example: shortest edit script has 5 edits
  assert.deepEqual(diffLines(a, b), segments);
});

await test('identical and empty texts', () => {
  assert.deepEqual(diffLines('', ''), []);
  assert.deepEqual(diffLines('x\n', 'x\n').map((s) => s.kind), ['equal']);
  assert.deepEqual(diffLines('', 'x\n').map((s) => s.kind), ['insert']);
  assert.deepEqual(splitLines('a\n\nb'), ['a\n', '\n', 'b']);
});

await test('past the edit-distance bound the diff is one coarse replacement but still exact', () => {
  const a = Array.from({ length: 50 }, (_, i) => `a${i}\n`).join('');
  const b = Array.from({ length: 50 }, (_, i) => `b${i}\n`).join('');
  const segments = diffLines(a, b, 10);
  assert.deepEqual(segments.map((s) => s.kind), ['delete', 'insert']);
  assert.equal(rebuild(segments, 'a'), a);
  assert.equal(rebuild(segments, 'b'), b);
});

await test('randomized texts always rebuild exactly', () => {
  let seed = 12345;
  const rand = (n) => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) % n);
  for (let round = 0; round < 200; round += 1) {
    const make = () => Array.from({ length: rand(12) }, () => 'xyz'[rand(3)] + (rand(5) === 0 ? '' : '\n')).join('');
    const a = make();
    const b = make();
    const segments = diffLines(a, b);
    assert.equal(rebuild(segments, 'a'), a);
    assert.equal(rebuild(segments, 'b'), b);
  }
});

// ---- store ----

const memory = () => {
  const map = new Map();
  return { map, get: async (k) => map.get(k), put: async (k, v) => void map.set(k, structuredClone(v)), delete: async (k) => void map.delete(k) };
};
const failing = (name) => {
  const error = Object.assign(new Error(name), { name });
  const reject = async () => {
    throw error;
  };
  return { get: reject, put: reject, delete: reject };
};

await test('the store round-trips a recorded snapshot and dedupes', async () => {
  const store = createSnapshotStore(memory());
  assert.equal((await store.record('k', { path: 'a', text: 'x', reason: 'save', at: 1 })).added, true);
  const again = await store.record('k', { path: 'a', text: 'x', reason: 'save', at: 2 });
  assert.equal(again.added, false);
  assert.equal((await store.load('k')).entries.length, 1);
});

await test('storage that throws is handled: load is empty, save names the reason', async () => {
  for (const name of ['SecurityError', 'QuotaExceededError']) {
    const store = createSnapshotStore(failing(name));
    assert.deepEqual(await store.load('k'), emptyHistory());
    assert.deepEqual(await store.save('k', emptyHistory()), { ok: false, reason: name === 'QuotaExceededError' ? 'quota' : 'unavailable' });
    const r = await store.record('k', { path: 'a', text: 'x', reason: 'save', at: 1 });
    assert.equal(r.ok, false);
    assert.equal(r.reason, name === 'QuotaExceededError' ? 'quota' : 'unavailable');
    await store.clear('k');
  }
  const noFactory = createSnapshotStore((await import('../../designer/web/snapshot-store.js')).indexedDbKeyValue(null));
  assert.deepEqual(await noFactory.load('k'), emptyHistory());
});

await test('a quota failure is retried with a smaller budget and can then succeed', async () => {
  const kv = memory();
  let quota = true;
  const put = kv.put;
  kv.put = async (k, v) => {
    if (quota) {
      quota = false;
      throw Object.assign(new Error('full'), { name: 'QuotaExceededError' });
    }
    return put(k, v);
  };
  const store = createSnapshotStore(kv);
  const r = await store.record('k', { path: 'a', text: 'x', reason: 'save', at: 1 });
  assert.equal(r.ok, true);
  assert.equal((await store.load('k')).entries.length, 1);
});

if (failures > 0) {
  process.stdout.write(`\n${failures} failed\n`);
  process.exit(1);
}
process.stdout.write('\nall passed\n');
