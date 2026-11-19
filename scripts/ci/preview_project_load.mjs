// SPDX-License-Identifier: MIT
//
// The studio's project load and save, tested without a browser (issue #391, D-S4).
//
//   node scripts/ci/preview_project_load.mjs
//
// `designer/web/project-io.js` takes a directory handle through its public methods only, so an
// in-memory fake stands in for `FileSystemDirectoryHandle`. What is asserted is the acceptance's
// load/save half: a folder and the zip of that folder give the same project, every limit refuses
// before a byte is read, a locked file is listed and never read, a hostile path is a named refusal,
// and Save writes only inside the root and notices a file that changed on disk.

import assert from 'node:assert/strict';

import {
  ProjectIoError,
  buildProject,
  deleteFiles,
  findConflicts,
  loadDirectory,
  loadZip,
  readViewOnly,
  writeFiles,
} from '../../designer/web/project-io.js';
import { applicableDraft, buildDraft, projectKey } from '../../designer/web/drafts.js';
import { LIMITS, PathError, isTextKind } from '../../designer/web/project-paths.js';
import { writeZip } from '../../designer/web/studio-zip.js';

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

const encoder = new TextEncoder();
const bytesOf = (text) => encoder.encode(text);

// --- an in-memory FileSystemDirectoryHandle ---------------------------------------------------------

let clock = 1000;
class FakeFile {
  constructor(bytes, lastModified) {
    this.bytes = bytes;
    this.lastModified = lastModified;
    this.reads = 0;
  }
  get size() {
    return this.bytes.length;
  }
  async arrayBuffer() {
    this.reads += 1;
    return this.bytes.buffer.slice(this.bytes.byteOffset, this.bytes.byteOffset + this.bytes.length);
  }
}
class FakeFileHandle {
  constructor(file) {
    this.kind = 'file';
    this.file = file;
  }
  async getFile() {
    return this.file;
  }
  async createWritable() {
    const handle = this;
    let text = '';
    return {
      async write(part) {
        text += part;
      },
      async close() {
        clock += 1;
        handle.file = new FakeFile(bytesOf(text), clock);
      },
    };
  }
}
class FakeDir {
  constructor() {
    this.kind = 'directory';
    this.children = new Map();
  }
  async *entries() {
    yield* this.children.entries();
  }
  async getDirectoryHandle(name, { create } = {}) {
    if (!this.children.has(name)) {
      if (!create) {
        throw Object.assign(new Error('missing'), { name: 'NotFoundError' });
      }
      this.children.set(name, new FakeDir());
    }
    return this.children.get(name);
  }
  async removeEntry(name) {
    if (!this.children.has(name)) {
      throw Object.assign(new Error('missing'), { name: 'NotFoundError' });
    }
    this.children.delete(name);
  }
  async getFileHandle(name, { create } = {}) {
    if (!this.children.has(name)) {
      if (!create) {
        throw Object.assign(new Error('missing'), { name: 'NotFoundError' });
      }
      this.children.set(name, new FakeFileHandle(new FakeFile(new Uint8Array(), (clock += 1))));
    }
    return this.children.get(name);
  }
}
function folder(files) {
  const root = new FakeDir();
  for (const [path, content] of Object.entries(files)) {
    const segments = path.split('/');
    let dir = root;
    for (const segment of segments.slice(0, -1)) {
      if (!dir.children.has(segment)) {
        dir.children.set(segment, new FakeDir());
      }
      dir = dir.children.get(segment);
    }
    const bytes = typeof content === 'string' ? bytesOf(content) : content;
    dir.children.set(segments.at(-1), new FakeFileHandle(new FakeFile(bytes, (clock += 1))));
  }
  return root;
}

const PROJECT = {
  'tessera.json': '{"app":{"entry":"ui/App.tsx"},"assets":{"fonts":["fonts/Inter.ttf"]}}',
  'ui/App.tsx': 'export function App() { return <View/>; }',
  'styles/theme.css': 'View { color: red; }',
  'fonts/Inter.ttf': new Uint8Array([0, 1, 2, 3, 4]),
  'src/native.cpp': '// generated, never edited',
};

// --- loading ---------------------------------------------------------------------------------------

await test('a folder loads: text files carry text, locked files are listed and never read', async () => {
  const root = folder(PROJECT);
  const { entries } = await loadDirectory(root);
  const byPath = Object.fromEntries(entries.map((entry) => [entry.path, entry]));
  assert.equal(byPath['ui/App.tsx'].kind, 'source');
  assert.equal(byPath['ui/App.tsx'].text, PROJECT['ui/App.tsx']);
  assert.equal(byPath['styles/theme.css'].kind, 'stylesheet');
  assert.equal(byPath['fonts/Inter.ttf'].kind, 'asset');
  assert.equal(byPath['fonts/Inter.ttf'].text, '');
  assert.equal(byPath['fonts/Inter.ttf'].size, 5);
  assert.equal(byPath['src/native.cpp'].kind, 'view-only');
  assert.equal(byPath['src/native.cpp'].text, '');
  const font = root.children.get('fonts').children.get('Inter.ttf').file;
  const cpp = root.children.get('src').children.get('native.cpp').file;
  assert.equal(font.reads, 0, 'a font is never read');
  assert.equal(cpp.reads, 0, 'C++ is not read at load; only opening it reads it');
});

await test('a locked text file opens view-only on demand, and its bytes never travel', async () => {
  const marker = 'LOCKED_MARKER_5f3a';
  const root = folder({ ...PROJECT, 'gen/AppBase.hpp': `// ${marker}\nstruct AppBase {};`, 'src/blob.cpp': new Uint8Array([0x41, 0, 0x42]) });
  const { entries } = await loadDirectory(root);
  const header = entries.find((entry) => entry.path === 'gen/AppBase.hpp');
  assert.equal(header.kind, 'view-only');
  assert.equal(header.text, '', 'the project copy of a locked file stays empty');
  const file = root.children.get('gen').children.get('AppBase.hpp').file;
  assert.equal(file.reads, 0);
  assert.deepEqual(await header.readView(), { text: `// ${marker}\nstruct AppBase {};` });
  assert.equal(file.reads, 1, 'read once, when opened');
  // A binary is a name and size only; an over-cap file is refused from its size without a read.
  assert.deepEqual(await entries.find((entry) => entry.path === 'src/blob.cpp').readView(), { note: 'binary' });
  assert.deepEqual(await readViewOnly(LIMITS.maxFileBytes + 1, async () => assert.fail('read')), { note: 'too-large' });
  assert.deepEqual(await readViewOnly(3, async () => new Uint8Array([0xff, 0xfe, 0xfd])), { note: 'binary' });
  // What a save writes and what the viewer is given are accepted text kinds only: a locked file is
  // refused by the writer, and no text entry carries its bytes.
  await assert.rejects(
    writeFiles(root, [{ path: 'gen/AppBase.hpp', text: marker }]),
    (error) => error instanceof ProjectIoError && error.code === 'not-accepted',
  );
  const sent = entries.filter((entry) => isTextKind(entry.kind)).map((entry) => entry.text).join('\n');
  assert.ok(!sent.includes(marker), 'a locked file is not among the text the viewer is given');
  assert.ok(!JSON.stringify(entries.map(({ readView, ...rest }) => rest)).includes(marker));
});

await test('a folder and the zip of that folder are the same project', async () => {
  const fromFolder = await loadDirectory(folder(PROJECT));
  const archive = writeZip(
    Object.entries(PROJECT).map(([path, content]) => ({
      path,
      bytes: typeof content === 'string' ? bytesOf(content) : content,
    })),
  );
  const fromZip = await loadZip(archive);
  const shape = (project) =>
    project.entries.map(({ path, kind, text }) => ({ path, kind, text })).sort((a, b) => (a.path < b.path ? -1 : 1));
  assert.deepEqual(shape(fromZip), shape(fromFolder));
});

await test('a zip of a folder (one wrapper directory) loads with that directory as the root', async () => {
  const archive = writeZip(
    Object.entries(PROJECT).map(([path, content]) => ({
      path: `my-app/${path}`,
      bytes: typeof content === 'string' ? bytesOf(content) : content,
    })),
  );
  const { entries } = await loadZip(archive);
  assert.ok(entries.some((entry) => entry.path === 'tessera.json'));
  assert.ok(entries.every((entry) => !entry.path.startsWith('my-app/')));
});

await test('node_modules is listed as one locked row and never walked', async () => {
  const root = folder({ ...PROJECT, 'node_modules/left-pad/index.js': 'x' });
  const { entries } = await loadDirectory(root);
  const rows = entries.filter((entry) => entry.path.startsWith('node_modules'));
  assert.deepEqual(rows.map((row) => row.path), ['node_modules']);
  assert.equal(rows[0].kind, 'other');
});

// --- limits and hostile input ------------------------------------------------------------------------

const rawOf = (path, size) => ({ path, size, read: async () => new Uint8Array(size) });

await test('a file over the per-file limit is refused by name before it is read', async () => {
  let read = false;
  await assert.rejects(
    buildProject([{ path: 'ui/Big.tsx', size: LIMITS.maxFileBytes + 1, read: async () => ((read = true), new Uint8Array()) }]),
    (error) => error instanceof ProjectIoError && error.code === 'limit' && error.limit === 'maxFileBytes',
  );
  assert.equal(read, false);
});

await test('more accepted files than the limit is refused', async () => {
  const raw = Array.from({ length: LIMITS.maxAcceptedFiles + 1 }, (_, index) => rawOf(`ui/F${index}.tsx`, 1));
  await assert.rejects(
    buildProject(raw),
    (error) => error.code === 'limit' && error.limit === 'maxAcceptedFiles',
  );
});

await test('more accepted bytes than the limit is refused', async () => {
  const count = Math.ceil(LIMITS.maxAcceptedBytes / LIMITS.maxFileBytes) + 1;
  const raw = Array.from({ length: count }, (_, index) => rawOf(`ui/F${index}.tsx`, LIMITS.maxFileBytes));
  await assert.rejects(
    buildProject(raw),
    (error) => error.code === 'limit' && error.limit === 'maxAcceptedBytes',
  );
});

await test('locked files do not count toward the accepted-file limit', async () => {
  const raw = Array.from({ length: LIMITS.maxAcceptedFiles + 50 }, (_, index) => rawOf(`data/f${index}.bin`, 10));
  const { entries } = await buildProject(raw);
  assert.equal(entries.length, LIMITS.maxAcceptedFiles + 50);
});

await test('more entries than the tree limit is refused', async () => {
  const raw = Array.from({ length: LIMITS.maxTreeEntries + 1 }, (_, index) => rawOf(`data/f${index}.bin`, 1));
  await assert.rejects(buildProject(raw), (error) => error.limit === 'maxTreeEntries');
});

await test('an oversized archive is refused before it is parsed', async () => {
  await assert.rejects(
    loadZip({ length: LIMITS.maxArchiveBytes + 1 }),
    (error) => error.limit === 'maxArchiveBytes',
  );
});

await test('a hostile path is a named refusal, and a duplicate after confinement is too', async () => {
  await assert.rejects(buildProject([rawOf('../evil.tsx', 1)]), (error) => error instanceof PathError);
  await assert.rejects(buildProject([rawOf('/abs.tsx', 1)]), (error) => error instanceof PathError);
  await assert.rejects(
    buildProject([rawOf('ui/a.tsx', 1), rawOf('ui//a.tsx', 1)]),
    (error) => error instanceof ProjectIoError && error.code === 'duplicate',
  );
});

// --- saving ------------------------------------------------------------------------------------------

await test('save writes into the folder, creating directories, and returns the new timestamps', async () => {
  const root = folder(PROJECT);
  const written = await writeFiles(root, [
    { path: 'ui/App.tsx', text: 'edited' },
    { path: 'ui/screens/Home.tsx', text: 'new file' },
  ]);
  assert.equal(root.children.get('ui').children.get('App.tsx').file.size, 6);
  assert.equal(root.children.get('ui').children.get('screens').children.get('Home.tsx').file.size, 8);
  assert.ok(written.get('ui/App.tsx') > 0);
});

await test('save refuses a path outside the root and a file the studio does not write', async () => {
  const root = folder(PROJECT);
  await assert.rejects(writeFiles(root, [{ path: '../x.tsx', text: '' }]), (error) => error instanceof PathError);
  await assert.rejects(
    writeFiles(root, [{ path: 'src/native.cpp', text: '' }]),
    (error) => error instanceof ProjectIoError && error.code === 'not-accepted',
  );
});

await test('a file that changed on disk since load is a conflict; an untouched one is not', async () => {
  const root = folder(PROJECT);
  const { baseline } = await loadDirectory(root);
  assert.deepEqual(await findConflicts(root, ['ui/App.tsx', 'styles/theme.css'], baseline), []);
  // Another editor saves ui/App.tsx.
  await writeFiles(root, [{ path: 'ui/App.tsx', text: 'changed elsewhere' }]);
  assert.deepEqual(await findConflicts(root, ['ui/App.tsx', 'styles/theme.css'], baseline), ['ui/App.tsx']);
});

await test('a new file whose path now exists on disk is a conflict; a deleted file is not', async () => {
  const root = folder(PROJECT);
  const { baseline } = await loadDirectory(root);
  assert.deepEqual(await findConflicts(root, ['ui/New.tsx'], baseline), []);
  await writeFiles(root, [{ path: 'ui/New.tsx', text: 'someone else made it' }]);
  assert.deepEqual(await findConflicts(root, ['ui/New.tsx'], baseline), ['ui/New.tsx']);
  root.children.get('ui').children.delete('App.tsx');
  assert.deepEqual(await findConflicts(root, ['ui/App.tsx'], baseline), []);
});

await test('save then reload keeps every edit', async () => {
  const root = folder(PROJECT);
  await writeFiles(root, [{ path: 'styles/theme.css', text: 'View { color: blue; }' }]);
  const { entries } = await loadDirectory(root);
  assert.equal(entries.find((entry) => entry.path === 'styles/theme.css').text, 'View { color: blue; }');
});

await test('deleting removes only accepted files inside the root; a missing file is fine', async () => {
  const root = folder(PROJECT);
  await deleteFiles(root, ['styles/theme.css', 'ui/gone.tsx', 'nowhere/x.tsx']);
  assert.equal(root.children.get('styles').children.has('theme.css'), false);
  assert.equal(root.children.get('ui').children.has('App.tsx'), true);
  await assert.rejects(deleteFiles(root, ['../x.tsx']), (error) => error instanceof PathError);
  await assert.rejects(
    deleteFiles(root, ['src/native.cpp']),
    (error) => error instanceof ProjectIoError && error.code === 'not-accepted',
  );
});

await test('a file deleted in the page but changed on disk is a conflict, not a silent delete', async () => {
  const root = folder(PROJECT);
  const { baseline } = await loadDirectory(root);
  await writeFiles(root, [{ path: 'styles/theme.css', text: 'edited elsewhere' }]);
  assert.deepEqual(await findConflicts(root, ['styles/theme.css'], baseline), ['styles/theme.css']);
});

const FILES = [
  { path: 'ui/App.tsx', text: 'one' },
  { path: 'styles/a.css', text: 'two' },
];

await test('a draft records what each file was edited from, and nothing when nothing is unsaved', () => {
  assert.equal(buildDraft(FILES, (path) => path, { dirty: [], added: [], removed: [] }), null);
  const draft = buildDraft(FILES, (path) => `edited ${path}`, {
    dirty: ['ui/App.tsx', 'ui/New.tsx'],
    added: ['ui/New.tsx'],
    removed: ['styles/a.css'],
    removedBase: new Map([['styles/a.css', 'two']]),
  });
  assert.deepEqual(draft.edits, [
    { path: 'ui/App.tsx', text: 'edited ui/App.tsx', base: 'one' },
    { path: 'ui/New.tsx', text: 'edited ui/New.tsx', base: null },
  ]);
  assert.deepEqual(draft.removed, [{ path: 'styles/a.css', base: 'two' }]);
  assert.equal(projectKey('folder', 'proj'), 'folder:proj');
});

await test('a draft is restored only onto the text it was edited from', () => {
  const draft = buildDraft(FILES, () => 'mine', {
    dirty: ['ui/App.tsx', 'ui/New.tsx'],
    added: ['ui/New.tsx'],
    removed: ['styles/a.css'],
    removedBase: new Map([['styles/a.css', 'two']]),
  });
  const same = applicableDraft(draft, FILES);
  assert.deepEqual([same.edits.map((e) => e.path), same.removed, same.skipped], [['ui/App.tsx', 'ui/New.tsx'], ['styles/a.css'], []]);
  // The file changed on disk since: its draft entry is skipped, never applied over the newer text.
  const moved = applicableDraft(draft, [
    { path: 'ui/App.tsx', text: 'changed elsewhere' },
    { path: 'styles/a.css', text: 'two' },
  ]);
  assert.deepEqual(moved.skipped, ['ui/App.tsx']);
  // A created file whose path now exists is skipped too.
  const taken = applicableDraft(draft, [...FILES, { path: 'ui/New.tsx', text: 'someone made it' }]);
  assert.ok(taken.skipped.includes('ui/New.tsx'));
});

await test('an oversized draft is not kept', () => {
  assert.equal(
    buildDraft(FILES, () => 'x'.repeat(LIMITS.maxFileBytes + 1), { dirty: ['ui/App.tsx'], added: [], removed: [] }),
    null,
  );
});

process.stdout.write(
  failures === 0 ? '\nproject load: all cases passed\n' : `\nproject load: ${failures} failed\n`,
);
if (failures > 0) {
  process.exitCode = 1;
}
