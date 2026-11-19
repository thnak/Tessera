// SPDX-License-Identifier: MIT
//
// The recent-projects list, tested without a browser (issue #391, owner direction 2026-11-17).
//
//   node scripts/ci/preview_recent_projects.mjs
//
// `designer/web/recent-folder.js` keeps a bounded list of folder handles in IndexedDB. The list policy
// (newest first, one entry per folder, at most ten) is the pure `mergeRecent`, driven here with plain
// objects; the IndexedDB calls are checked only for the case Node can produce honestly: no storage at
// all, where every call must degrade to "nothing remembered" and never throw.
import assert from 'node:assert/strict';

import {
  MAX_RECENT,
  forgetAllFolders,
  forgetFolder,
  mergeRecent,
  recallFolders,
  rememberFolder,
} from '../../designer/web/recent-folder.js';

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

// A handle whose identity is its `id`, like `isSameEntry` on two handles to one directory.
const folder = (name, id = name) => ({ name, id, isSameEntry: async (other) => other.id === id });
const names = (list) => list.map((entry) => entry.name);

await test('the newest folder is first and carries its open time', async () => {
  let list = await mergeRecent([], folder('a'), 10);
  list = await mergeRecent(list, folder('b'), 20);
  assert.deepEqual(names(list), ['b', 'a']);
  assert.deepEqual(list.map((entry) => entry.openedAt), [20, 10]);
});

await test('opening a folder again moves it to the front instead of listing it twice', async () => {
  let list = await mergeRecent([], folder('a'), 1);
  list = await mergeRecent(list, folder('b'), 2);
  list = await mergeRecent(list, folder('a'), 3);
  assert.deepEqual(names(list), ['a', 'b']);
  assert.equal(list[0].openedAt, 3);
});

await test('two folders with the same name are two entries when they are different directories', async () => {
  let list = await mergeRecent([], folder('app', 'one'), 1);
  list = await mergeRecent(list, folder('app', 'two'), 2);
  assert.equal(list.length, 2);
});

await test('the list is bounded and the oldest falls off', async () => {
  let list = [];
  for (let index = 0; index < MAX_RECENT + 5; index += 1) {
    list = await mergeRecent(list, folder(`p${index}`), index);
  }
  assert.equal(MAX_RECENT, 10);
  assert.equal(list.length, MAX_RECENT);
  assert.equal(list[0].name, `p${MAX_RECENT + 4}`);
  assert.ok(!names(list).includes('p0'));
});

await test('a handle with no isSameEntry is compared by name', async () => {
  let list = await mergeRecent([], { name: 'x' }, 1);
  list = await mergeRecent(list, { name: 'x' }, 2);
  assert.equal(list.length, 1);
});

await test('with no storage every call degrades to nothing remembered and never throws', async () => {
  assert.equal(typeof globalThis.indexedDB, 'undefined');
  await rememberFolder(folder('a'));
  await forgetFolder(folder('a'));
  await forgetAllFolders();
  assert.deepEqual(await recallFolders(), []);
});

if (failures > 0) {
  process.stdout.write(`\nrecent projects: ${failures} case(s) failed\n`);
  process.exit(1);
}
process.stdout.write('\nrecent projects: all cases passed\n');
