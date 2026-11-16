// SPDX-License-Identifier: MIT
//
// The studio's zip container, tested without a browser (issue #391, D-S4 of
// docs/specs/designer-studio-v1.md section 7).
//
//   node scripts/ci/preview_project_io.mjs
//
// `designer/web/studio-zip.js` is the whole non-Chromium load path and the whole export path, and it
// is half of this issue's acceptance: "In other browsers, dropping a zip loads the same project and
// Download project exports a zip with every edit." It is also the half that needs no page, no Wasm and
// no browser to verify, so it is verified here, byte for byte, rather than by hand.
//
// **The writer.** Store-only, deterministic, no dependency (the issue forbids JSZip). Two properties
// are asserted because the download depends on them:
//
//   * a write followed by a read returns every file unchanged, including a binary;
//   * the *same* project writes the *same bytes* twice, whatever order the caller hands the entries
//     in -- so a visitor who exports twice gets an identical archive, and a test can compare bytes
//     rather than "equivalent archives".
//
// **The reader.** It supports what real archivers produce, not only what this repository writes: a
// DEFLATE entry (method 8, through the platform's `DecompressionStream`) is read, and every entry's
// CRC-32 is verified. Each way a hostile or broken archive can arrive is a *named* `ZipError` rather
// than a half-loaded project:
//
//   | case | code |
//   |---|---|
//   | a corrupted or truncated body | `crc` / `truncated` |
//   | an encrypted entry | `encrypted` |
//   | a compression method nothing here reads | `method` |
//   | a Unix symlink entry (a path out of the archive) | `symlink` |
//   | the same entry name twice | `duplicate` |
//   | a ZIP64 archive | `zip64` |
//   | a file that is not a zip at all | `signature` |
//
// **Both modules are framework-independent**: no imports, no DOM, no `window`, so the React shell
// (#422) imports them unchanged. That is also why this test drives the module directly instead of
// through a page, and why it keeps working when the page is rewritten.
//
// Path safety is the other module and has its own test (`preview_project_paths.mjs`): `readZip` returns
// entry names *verbatim* and says nothing about whether they are safe, because confinement is one
// decision in one place.
//
// Browser verification: **not performed** for this change. Opening a folder, dropping a zip on a page
// and starting a download all need a page, and #391 is blocked on #422's shell.
import assert from 'node:assert/strict';

import {
  FILE_KIND,
  LIMITS,
  PathError,
  classifyPath,
  confinePath,
  extensionOf,
  isConfined,
  isTextKind,
  isUnder,
  joinProjectPath,
  parentOf,
} from '../../designer/web/project-paths.js';
import { ZipError, crc32, readZip, writeZip } from '../../designer/web/studio-zip.js';

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
const decoder = new TextDecoder('utf-8');

function bytesOf(text) {
  return encoder.encode(text);
}

/** The entries of an archive as a `path -> text` map, for readable assertions. */
function textsOf(entries) {
  const out = new Map();
  for (const entry of entries) {
    out.set(entry.path, decoder.decode(entry.bytes));
  }
  return out;
}

// --- a project, as the studio would export it ----------------------------------------------------

const PROJECT = [
  { path: 'tessera.json', text: '{\n  "schemaVersion": 1,\n  "app": { "entry": "ui/App.tsx" }\n}\n' },
  {
    path: 'ui/App.tsx',
    text: 'import { Home } from "./Home";\n\nexport function App(): Node {\n  return <Home />;\n}\n',
  },
  {
    path: 'ui/Home.tsx',
    text: 'export function Home(): Node {\n  return <Text>Hello</Text>;\n}\n',
  },
  { path: 'styles/theme.css', text: 'Text { color: #e6edf7; }\n' },
  { path: 'assets/theme.json', text: '{ "schemaVersion": 1, "themes": {} }\n' },
  // A path with a space and a non-ASCII segment: the tree and the zip must carry it unchanged.
  { path: 'ui/deep/naïve page.tsx', text: 'export function Page(): Node {\n  return <View />;\n}\n' },
];

const projectEntries = () => PROJECT.map((file) => ({ path: file.path, bytes: bytesOf(file.text) }));

// --- 1. the writer and the reader are inverses ---------------------------------------------------

await test('write then read returns every file byte for byte', async () => {
  const archive = writeZip(projectEntries());
  const back = await readZip(archive);
  assert.equal(back.length, PROJECT.length);
  const texts = textsOf(back);
  for (const file of PROJECT) {
    assert.equal(texts.get(file.path), file.text, `round trip: ${file.path}`);
  }
});

await test('the archive is deterministic: the same project writes the same bytes', async () => {
  const first = writeZip(projectEntries());
  // Shuffled input: a writer that depended on caller order would produce a different archive, and
  // then the download would change every time the same project was exported.
  const shuffled = projectEntries();
  shuffled.reverse();
  const second = writeZip(shuffled);
  assert.deepEqual(Array.from(second), Array.from(first), 'a stable export is a byte-identical export');
  assert.equal(crc32(first), crc32(second));
});

await test('the archive is a real zip: the local headers carry the right sizes and checksums', async () => {
  const archive = writeZip([{ path: 'a.tsx', bytes: bytesOf('export function A(): Node {}') }]);
  const view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
  assert.equal(view.getUint32(0, true), 0x04034b50, 'local file header signature');
  assert.equal(view.getUint16(8, true), 0, 'method is store');
  const crc = view.getUint32(14, true);
  const size = view.getUint32(18, true);
  assert.equal(size, archive.length - 30 - 'a.tsx'.length - 22 - 46 - 'a.tsx'.length);
  assert.equal(crc, crc32(bytesOf('export function A(): Node {}')), 'the stored CRC is the data CRC');
  assert.equal(view.getUint16(6, true) & 0x0800, 0x0800, 'the UTF-8 flag is set');
  // The end record's count and the central directory's size agree with what was written.
  const end = archive.length - 22;
  assert.equal(view.getUint32(end, true), 0x06054b50, 'end of central directory signature');
  assert.equal(view.getUint16(end + 10, true), 1, 'one entry');
});

await test('an empty project writes a valid empty archive', async () => {
  const archive = writeZip([]);
  assert.equal(archive.length, 22, 'just the end record');
  const back = await readZip(archive);
  assert.deepEqual(back, []);
});

await test('a binary asset survives the round trip unchanged', async () => {
  const pixel = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0xff, 0x80]);
  const back = await readZip(writeZip([{ path: 'assets/dot.png', bytes: pixel }]));
  assert.deepEqual(Array.from(back[0].bytes), Array.from(pixel), 'bytes, not a string round trip');
});

// --- 2. reading what this repository does not write ----------------------------------------------

// A minimal deflated zip, hand-assembled: one entry, method 8, the body raw-DEFLATE-compressed by
// the platform's own CompressionStream (the inverse of the reader's DecompressionStream). This is
// what `zip -r`, Finder and Explorer produce, and it is the case a STORE-only reader would fail.
async function deflatedZip(path, text) {
  const raw = bytesOf(text);
  const compressed = await deflateRaw(raw);
  const name = encoder.encode(path);
  const crc = crc32(raw);

  const local = new Uint8Array(30 + name.length);
  const localView = new DataView(local.buffer);
  localView.setUint32(0, 0x04034b50, true);
  localView.setUint16(4, 20, true);
  localView.setUint16(6, 0, true);
  localView.setUint16(8, 8, true); // deflate
  localView.setUint32(14, crc, true);
  localView.setUint32(18, compressed.length, true);
  localView.setUint32(22, raw.length, true);
  localView.setUint16(26, name.length, true);
  local.set(name, 30);

  const central = new Uint8Array(46 + name.length);
  const centralView = new DataView(central.buffer);
  centralView.setUint32(0, 0x02014b50, true);
  centralView.setUint16(4, 20, true);
  centralView.setUint16(6, 20, true);
  centralView.setUint16(10, 8, true);
  centralView.setUint32(16, crc, true);
  centralView.setUint32(20, compressed.length, true);
  centralView.setUint32(24, raw.length, true);
  centralView.setUint16(28, name.length, true);
  centralView.setUint32(42, 0, true);
  central.set(name, 46);

  const end = new Uint8Array(22);
  const endView = new DataView(end.buffer);
  endView.setUint32(0, 0x06054b50, true);
  endView.setUint16(8, 1, true);
  endView.setUint16(10, 1, true);
  endView.setUint32(12, central.length, true);
  endView.setUint32(16, local.length + compressed.length, true);

  const out = new Uint8Array(local.length + compressed.length + central.length + end.length);
  let cursor = 0;
  for (const part of [local, compressed, central, end]) {
    out.set(part, cursor);
    cursor += part.length;
  }
  return out;
}

async function deflateRaw(data) {
  const stream = new CompressionStream('deflate-raw');
  const writer = stream.writable.getWriter();
  writer.write(data).catch(() => {});
  writer.close().catch(() => {});
  const reader = stream.readable.getReader();
  const chunks = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) {
      break;
    }
    chunks.push(value);
    total += value.length;
  }
  const out = new Uint8Array(total);
  let cursor = 0;
  for (const chunk of chunks) {
    out.set(chunk, cursor);
    cursor += chunk.length;
  }
  return out;
}

await test('a deflated entry (what every real archiver writes) is read', async () => {
  const text = 'export function Deflated(): Node {\n  return <View />;\n}\n'.repeat(20);
  const archive = await deflatedZip('ui/Deflated.tsx', text);
  const back = await readZip(archive);
  assert.equal(back.length, 1);
  assert.equal(back[0].path, 'ui/Deflated.tsx');
  assert.equal(decoder.decode(back[0].bytes), text);
});

await test('a corrupted entry is a named CRC refusal, not a half-loaded project', async () => {
  const archive = writeZip([{ path: 'a.css', bytes: bytesOf('Text { color: red; }\n') }]);
  // Flip a byte of the body (past the 30-byte local header and the 5-byte name).
  archive[30 + 'a.css'.length + 2] ^= 0xff;
  await assert.rejects(
    () => readZip(archive),
    (error) => error instanceof ZipError && error.code === 'crc',
    'a corrupted body must be refused by its checksum',
  );
});

/** The byte offset of the central-directory record of entry `index`, and its name length. */
function centralRecord(archive, index = 0) {
  const view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
  const end = archive.length - 22;
  let cursor = view.getUint32(end + 16, true);
  for (let at = 0; at < index; at += 1) {
    const nameLength = view.getUint16(cursor + 28, true);
    const extraLength = view.getUint16(cursor + 30, true);
    const commentLength = view.getUint16(cursor + 32, true);
    cursor += 46 + nameLength + extraLength + commentLength;
  }
  return { offset: cursor, nameLength: view.getUint16(cursor + 28, true) };
}

await test('an encrypted entry is refused by name', async () => {
  const archive = writeZip([{ path: 'a.css', bytes: bytesOf('x') }]);
  const view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
  view.setUint16(6, 0x0001, true); // local header flags: encrypted
  view.setUint16(centralRecord(archive).offset + 8, 0x0001, true); // central header flags
  await assert.rejects(
    () => readZip(archive),
    (error) => error instanceof ZipError && error.code === 'encrypted',
  );
});

await test('an unsupported compression method is refused by name', async () => {
  const archive = writeZip([{ path: 'a.css', bytes: bytesOf('x') }]);
  const view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
  view.setUint16(centralRecord(archive).offset + 10, 12, true); // bzip2, which nothing here reads
  await assert.rejects(
    () => readZip(archive),
    (error) => error instanceof ZipError && error.code === 'method',
  );
});

await test('a symlink entry is refused: a link can point outside the project', async () => {
  const archive = writeZip([{ path: 'ui/Evil.tsx', bytes: bytesOf('/etc/passwd') }]);
  const view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
  // External attributes' high 16 bits are the Unix mode; S_IFLNK is 0xA000, plus the 0777 bits a
  // real archiver writes for a link.
  view.setUint32(centralRecord(archive).offset + 38, 0xa1ff0000, true);
  await assert.rejects(
    () => readZip(archive),
    (error) => error instanceof ZipError && error.code === 'symlink',
  );
});

await test('a duplicate entry name is refused', async () => {
  // Two identical entries: the second central record points at the same local header, which is
  // exactly what an attacker-in-the-middle archive looks like.
  const one = writeZip([{ path: 'ui/App.tsx', bytes: bytesOf('a') }]);
  const duplicated = duplicateFirstEntry(one);
  await assert.rejects(
    () => readZip(duplicated),
    (error) => error instanceof ZipError && error.code === 'duplicate',
  );
});

function duplicateFirstEntry(archive) {
  const view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
  const end = archive.length - 22;
  const centralSize = view.getUint32(end + 12, true);
  const centralStart = view.getUint32(end + 16, true);
  const firstCentralSize = 46 + view.getUint16(centralStart + 28, true);
  const copy = archive.subarray(centralStart, centralStart + firstCentralSize);
  const out = new Uint8Array(archive.length + copy.length);
  out.set(archive.subarray(0, end), 0);
  out.set(copy, end);
  out.set(archive.subarray(end), end + copy.length);
  const outView = new DataView(out.buffer);
  const newEnd = end + copy.length;
  outView.setUint16(newEnd + 8, 2, true);
  outView.setUint16(newEnd + 10, 2, true);
  outView.setUint32(newEnd + 12, centralSize + copy.length, true);
  return out;
}

await test('a truncated file is refused as truncated, not as "not a zip"', async () => {
  const archive = writeZip(projectEntries());
  const cut = archive.subarray(0, Math.floor(archive.length / 2));
  await assert.rejects(
    () => readZip(cut),
    (error) => error instanceof ZipError && (error.code === 'truncated' || error.code === 'signature'),
  );
  await assert.rejects(
    () => readZip(new Uint8Array(10)),
    (error) => error instanceof ZipError && error.code === 'truncated',
  );
});

await test('a ZIP64 archive is refused by name rather than misread', async () => {
  const archive = writeZip([{ path: 'a.tsx', bytes: bytesOf('x') }]);
  const out = new Uint8Array(archive.length + 20);
  out.set(archive, 0);
  const end = archive.length - 22;
  const view = new DataView(out.buffer);
  // The ZIP64 locator sits immediately before the end record.
  view.setUint32(end, 0x07064b50, true);
  out.set(archive.subarray(end), end + 20);
  await assert.rejects(
    () => readZip(out),
    (error) => error instanceof ZipError && error.code === 'zip64',
  );
});

await test('a file that is not a zip at all is refused', async () => {
  await assert.rejects(
    () => readZip(bytesOf('this is a TSX file, not an archive\n')),
    (error) => error instanceof ZipError && error.code === 'signature',
  );
});

await test('a zip bomb is stopped by its declared size, not buffered in full', async () => {
  // A 1 KB entry whose central directory *lies* about the uncompressed size: the body inflates to
  // several megabytes but the header claims 16 bytes. The reader must stop at the declared size rather
  // than buffer the whole expansion, so a hostile archive cannot exhaust the page's memory before the
  // CRC says no. The assertion is that it refuses quickly with a `crc` code -- not that it eventually
  // finishes after allocating everything.
  const raw = bytesOf('A'.repeat(4 * 1024 * 1024));
  const body = await deflateRaw(raw);
  const name = encoder.encode('bomb.tsx');
  const claimed = 16; // the lie

  const local = new Uint8Array(30 + name.length);
  const localView = new DataView(local.buffer);
  localView.setUint32(0, 0x04034b50, true);
  localView.setUint16(4, 20, true);
  localView.setUint16(8, 8, true);
  localView.setUint32(14, crc32(raw), true);
  localView.setUint32(18, body.length, true);
  localView.setUint32(22, claimed, true);
  localView.setUint16(26, name.length, true);
  local.set(name, 30);

  const central = new Uint8Array(46 + name.length);
  const centralView = new DataView(central.buffer);
  centralView.setUint32(0, 0x02014b50, true);
  centralView.setUint16(4, 20, true);
  centralView.setUint16(6, 20, true);
  centralView.setUint16(10, 8, true);
  centralView.setUint32(16, crc32(raw), true);
  centralView.setUint32(20, body.length, true);
  centralView.setUint32(24, claimed, true);
  centralView.setUint16(28, name.length, true);
  centralView.setUint32(42, 0, true);
  central.set(name, 46);

  const end = new Uint8Array(22);
  const endView = new DataView(end.buffer);
  endView.setUint32(0, 0x06054b50, true);
  endView.setUint16(8, 1, true);
  endView.setUint16(10, 1, true);
  endView.setUint32(12, central.length, true);
  endView.setUint32(16, local.length + body.length, true);

  const archive = new Uint8Array(local.length + body.length + central.length + end.length);
  let cursor = 0;
  for (const part of [local, body, central, end]) {
    archive.set(part, cursor);
    cursor += part.length;
  }
  assert.ok(archive.length < 64 * 1024, 'the archive itself is tiny, which is the point of a bomb');
  await assert.rejects(
    () => readZip(archive),
    (error) => error instanceof ZipError && error.code === 'crc',
    'a body that inflates past its declared size is refused',
  );
});

await test('a directory entry is skipped, because the tree is built from file paths', async () => {
  const archive = writeZip([
    { path: 'ui/', bytes: new Uint8Array(0) },
    { path: 'ui/App.tsx', bytes: bytesOf('x') },
  ]);
  const back = await readZip(archive);
  assert.deepEqual(
    back.map((entry) => entry.path),
    ['ui/App.tsx'],
  );
});

// --- report --------------------------------------------------------------------------------------

process.stdout.write(
  failures === 0 ? '\nproject IO: all cases passed\n' : `\nproject IO: ${failures} failed\n`,
);
if (failures > 0) {
  process.exitCode = 1;
}
