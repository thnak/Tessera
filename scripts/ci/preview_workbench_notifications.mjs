// SPDX-License-Identifier: MIT
//
// The notifications service's acceptance check (issue #559; `docs/specs/studio-workbench-v1.md` section 6).
//
//   node scripts/ci/preview_workbench_notifications.mjs
//
// Pure Node with an injected clock: the queue (at most three toasts, the rest wait), expiry per severity,
// dedupe by key inside a window, the history cap and unread count, do-not-disturb (toasts suppressed,
// history kept), dismissal, and malformed input refused.

import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { assert, finish, test } from './mock_test_support.mjs';

const WORKBENCH = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'designer', 'web', 'src', 'workbench');
const { createNotifications } = await import(pathToFileURL(resolve(WORKBENCH, 'notifications.js')).href);

function service(options = {}) {
  const clock = { at: 1_000_000 };
  return { clock, notes: createNotifications({ now: () => clock.at, ...options }) };
}
const texts = (list) => list.map((entry) => entry.message);

await test('notify: a toast and a history entry, newest first, unread until read', () => {
  const { notes } = service();
  const first = notes.notify({ message: 'Saved 2 files' });
  const second = notes.notify({ message: 'Link copied', severity: 'info' });
  assert.ok(first !== null && second !== null && first !== second);
  const view = notes.snapshot();
  assert.deepEqual(texts(view.toasts), ['Saved 2 files', 'Link copied']);
  assert.deepEqual(texts(view.history), ['Link copied', 'Saved 2 files']);
  assert.equal(view.unread, 2);
  notes.markRead();
  assert.equal(notes.snapshot().unread, 0);
});

await test('notify: malformed input is refused and changes nothing', () => {
  const { notes } = service();
  const before = notes.version();
  for (const bad of [null, {}, { message: '' }, { message: '   ' }, { message: 5 }, { message: 'x', severity: 'fatal' }]) {
    assert.equal(notes.notify(bad), null);
  }
  assert.equal(notes.version(), before);
  const long = notes.notify({ message: 'x'.repeat(2000) });
  assert.equal(notes.snapshot().history[0].message.length, 500);
  assert.ok(long !== null);
});

await test('queue: at most three toasts show, the rest wait and appear as room is made', () => {
  const { notes } = service();
  const ids = ['a', 'b', 'c', 'd', 'e'].map((message) => notes.notify({ message }));
  let view = notes.snapshot();
  assert.deepEqual(texts(view.toasts), ['a', 'b', 'c']);
  assert.equal(view.queued, 2);
  assert.equal(view.history.length, 5, 'every one is in the history');
  notes.dismiss(ids[0]);
  view = notes.snapshot();
  assert.deepEqual(texts(view.toasts), ['b', 'c', 'd']);
  assert.equal(view.queued, 1);
  notes.dismissAll();
  assert.deepEqual(notes.snapshot().toasts, []);
  assert.equal(notes.snapshot().history.length, 5, 'dismissing a toast keeps the history');
});

await test('expiry: info 5 s, warning 8 s, an action keeps it at least 10 s, an error stays', () => {
  const { notes, clock } = service();
  notes.notify({ message: 'info' });
  notes.notify({ message: 'warn', severity: 'warning' });
  notes.notify({ message: 'act', actions: [{ label: 'Undo', command: 'edit.undo' }] });
  notes.notify({ message: 'bad', severity: 'error' });
  clock.at += 4_999;
  assert.equal(notes.tick(), false);
  clock.at += 2;
  assert.equal(notes.tick(), true);
  assert.deepEqual(texts(notes.snapshot().toasts), ['warn', 'act', 'bad'], 'info expired and the queued error took its place');
  clock.at += 3_000;
  notes.tick();
  assert.deepEqual(texts(notes.snapshot().toasts), ['act', 'bad'], 'warning expired at 8 s');
  clock.at += 2_100;
  notes.tick();
  assert.deepEqual(texts(notes.snapshot().toasts), ['bad'], 'the action toast lived 10 s');
  clock.at += 600_000;
  assert.equal(notes.tick(), false);
  assert.deepEqual(texts(notes.snapshot().toasts), ['bad'], 'an error never expires');
  assert.equal(notes.snapshot().history.length, 4, 'expiry never touches the history');
});

await test('dedupe: the same key inside the window bumps a counter and re-shows the toast', () => {
  const { notes, clock } = service();
  const first = notes.notify({ message: 'Saved 2 files', key: 'project-note' });
  clock.at += 1_000;
  const again = notes.notify({ message: 'Saved 3 files', key: 'project-note' });
  assert.equal(again, first);
  let view = notes.snapshot();
  assert.equal(view.history.length, 1);
  assert.equal(view.history[0].count, 2);
  assert.equal(view.history[0].message, 'Saved 3 files', 'the latest text wins');
  assert.equal(view.toasts.length, 1);
  notes.dismiss(first);
  assert.equal(notes.snapshot().toasts.length, 0);
  clock.at += 1_000;
  notes.notify({ message: 'Saved 3 files', key: 'project-note' });
  view = notes.snapshot();
  assert.equal(view.history.length, 1, 'still one entry');
  assert.equal(view.history[0].count, 3);
  assert.equal(view.toasts.length, 1, 'the dismissed toast came back');
  clock.at += 11_000;
  notes.notify({ message: 'Saved 3 files', key: 'project-note' });
  assert.equal(notes.snapshot().history.length, 2, 'outside the window it is a new entry');
  const defaultKey = service().notes;
  defaultKey.notify({ message: 'same' });
  defaultKey.notify({ message: 'same' });
  defaultKey.notify({ message: 'same', severity: 'warning' });
  assert.equal(defaultKey.snapshot().history.length, 2, 'the default key is severity plus message');
});

await test('history: capped at the limit, the oldest dropped; clear empties everything', () => {
  const { notes, clock } = service({ historyLimit: 5 });
  for (let i = 0; i < 8; i += 1) {
    clock.at += 20_000;
    notes.notify({ message: `n${i}` });
  }
  const view = notes.snapshot();
  assert.deepEqual(texts(view.history), ['n7', 'n6', 'n5', 'n4', 'n3']);
  notes.clearHistory();
  assert.deepEqual([notes.snapshot().history.length, notes.snapshot().toasts.length, notes.snapshot().unread], [0, 0, 0]);
});

await test('do not disturb: toasts suppressed and cleared, history and unread kept', () => {
  const { notes } = service();
  notes.notify({ message: 'before' });
  notes.setDoNotDisturb(true);
  assert.equal(notes.snapshot().dnd, true);
  assert.deepEqual(notes.snapshot().toasts, [], 'the toast on screen is hidden');
  notes.notify({ message: 'during' });
  notes.notify({ message: 'worse', severity: 'error' });
  const view = notes.snapshot();
  assert.deepEqual(view.toasts, []);
  assert.deepEqual(texts(view.history), ['worse', 'during', 'before']);
  assert.equal(view.unread, 3);
  notes.setDoNotDisturb(false);
  notes.notify({ message: 'after' });
  assert.deepEqual(texts(notes.snapshot().toasts), ['after']);
});

await test('actions: kept (at most three, well formed) with the entry, ready for the registry', () => {
  const { notes } = service();
  notes.notify({
    message: 'Layout reset',
    actions: [{ label: 'Undo', command: 'view.resetLayout' }, { label: 'bad' }, { label: 'Open', command: 'view.explorer', args: { x: 1 } }, { label: 'a', command: 'a.b' }, { label: 'b', command: 'b.c' }],
  });
  const [entry] = notes.snapshot().toasts;
  assert.deepEqual(entry.actions.map((action) => action.label), ['Undo', 'Open', 'a']);
  assert.deepEqual(entry.actions[1].args, { x: 1 });
});

await test('subscribe: one notification per change, none for a no-op', () => {
  const { notes } = service();
  let calls = 0;
  const stop = notes.subscribe(() => {
    calls += 1;
  });
  notes.notify({ message: 'a' });
  assert.equal(calls, 1);
  notes.dismissAll();
  notes.dismissAll();
  assert.equal(calls, 2, 'a second dismissAll changes nothing');
  notes.markRead();
  notes.markRead();
  assert.equal(calls, 3);
  stop();
  notes.notify({ message: 'b' });
  assert.equal(calls, 3);
});

finish('preview_workbench_notifications');
