// SPDX-License-Identifier: MIT
//
// Split editor groups (#1025), the pure model: which tab is in which group, what a split, a move, a join and
// a close do, and that focus bookkeeping (`settle`) never loses a group's own file. Plain Node, no UI, no build:
//
//   node scripts/ci/preview_editor_groups.mjs

import {
  activeAfterClose, activeOf, canSplit, groupOf, groupTabs, isSplit, joinGroups, moveTab, settle, splitActive,
} from '../../designer/web/editor-groups.js';
import { assert, finish, test } from './mock_test_support.mjs';

const state = (open, active = open[0], extra = {}) => ({ open, active, side: [], mainActive: active, sideActive: '', ...extra });
/** A workspace update the way the hook applies it: through `settle`. */
const apply = (prev, next) => settle(prev, next);

await test('one group until split; split needs two tabs', () => {
  assert.equal(isSplit(state(['a'])), false);
  assert.equal(canSplit(state(['a'])), false);
  assert.equal(canSplit(state(['a', 'b'])), true);
  assert.deepEqual(splitActive(state(['a'])), state(['a']));
});

await test('split moves the focused tab to a second group and focuses it', () => {
  const split = apply(state(['a', 'b', 'c'], 'b'), splitActive(state(['a', 'b', 'c'], 'b')));
  assert.deepEqual(groupTabs(split), { main: ['a', 'c'], side: ['b'] });
  assert.equal(split.active, 'b');
  assert.equal(activeOf(split, 'side'), 'b');
  assert.equal(activeOf(split, 'main'), 'c');
  assert.equal(canSplit(split), false, 'only one split level');
});

await test('per-group active survives focus moving between groups', () => {
  const start = state(['a', 'b', 'c'], 'a');
  let now = apply(start, moveTab(start, 'c', 'side'));
  assert.deepEqual([now.active, activeOf(now, 'main'), activeOf(now, 'side')], ['c', 'a', 'c']);
  const focusMain = apply(now, { ...now, active: 'b' });
  assert.equal(activeOf(focusMain, 'side'), 'c', 'the side group remembers its file');
  assert.equal(activeOf(focusMain, 'main'), 'b');
  const back = apply(focusMain, { ...focusMain, active: 'c' });
  assert.equal(activeOf(back, 'main'), 'b', 'and the main group remembers its own');
});

await test('moving a tab between groups changes only membership', () => {
  const start = state(['a', 'b', 'c', 'd'], 'a');
  const split = apply(start, moveTab(start, 'c', 'side'));
  const more = apply(split, moveTab(split, 'd', 'side', 0));
  assert.deepEqual(groupTabs(more), { main: ['a', 'b'], side: ['d', 'c'] });
  const back = apply(more, moveTab(more, 'c', 'main', 1));
  assert.deepEqual(groupTabs(back), { main: ['a', 'c', 'b'], side: ['d'] });
  assert.deepEqual([...back.open].sort(), ['a', 'b', 'c', 'd'], 'no tab is created or lost');
  assert.equal(back.active, 'c', 'the moved tab takes focus');
});

await test('moving inside a group reorders it', () => {
  const start = state(['a', 'b', 'c'], 'a');
  assert.deepEqual(groupTabs(moveTab(start, 'a', 'main', 2)).main, ['b', 'c', 'a']);
});

await test('emptying a group collapses the split', () => {
  const start = state(['a', 'b'], 'a');
  const split = apply(start, moveTab(start, 'b', 'side'));
  const merged = apply(split, moveTab(split, 'b', 'main'));
  assert.equal(isSplit(merged), false);
  const flipped = apply(split, moveTab(split, 'a', 'side'));
  assert.equal(isSplit(flipped), false, 'main cannot be empty beside a populated second group');
  assert.deepEqual([...flipped.open].sort(), ['a', 'b']);
});

await test('closing keeps focus inside the closed tab\'s own group', () => {
  const start = state(['a', 'b', 'c', 'd'], 'a');
  const split = apply(start, moveTab(moveTab(start, 'c', 'side'), 'd', 'side'));
  assert.deepEqual(groupTabs(split), { main: ['a', 'b'], side: ['c', 'd'] });
  const focusD = { ...split, active: 'd', sideActive: 'd' };
  assert.equal(activeAfterClose(focusD, 'd'), 'c');
  assert.equal(activeAfterClose(focusD, 'a'), 'd', 'closing an unfocused tab keeps the focus');
  const closed = apply(focusD, { ...focusD, open: ['a', 'b', 'c'], active: activeAfterClose(focusD, 'd') });
  assert.equal(closed.active, 'c');
  const last = apply(closed, { ...closed, open: ['a', 'b'], active: activeAfterClose(closed, 'c') });
  assert.equal(isSplit(last), false, 'closing the last second-group tab ends the split');
  assert.equal(last.active, 'b');
});

await test('join merges the groups and keeps the order and focus', () => {
  const start = state(['a', 'b', 'c'], 'a');
  const split = apply(start, moveTab(start, 'b', 'side'));
  const joined = joinGroups(split);
  assert.deepEqual(groupTabs(joined), { main: ['a', 'c', 'b'], side: [] });
  assert.equal(joined.active, 'b');
});

await test('settle drops tabs that are no longer open (delete, rename)', () => {
  const start = state(['a', 'b', 'c'], 'a');
  const split = apply(start, moveTab(start, 'c', 'side'));
  const gone = apply(split, { ...split, open: ['a', 'b'], active: 'a' });
  assert.equal(isSplit(gone), false);
  assert.equal(groupOf(gone, 'a'), 'main');
});

await test('settle returns the same object when nothing changed (no extra render)', () => {
  const start = state(['a', 'b'], 'a');
  assert.equal(settle(start, start), start);
  const same = { ...start };
  assert.equal(settle(start, same), same);
});

await test('the model owns no text: a move cannot touch dirty or buffers', () => {
  const start = { ...state(['a', 'b'], 'a'), dirty: ['b'], files: [1, 2] };
  const moved = moveTab(start, 'b', 'side');
  assert.deepEqual(moved.dirty, ['b']);
  assert.equal(moved.files, start.files);
});

finish('editor groups');
