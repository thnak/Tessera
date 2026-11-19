// SPDX-License-Identifier: MIT
//
// The project package (zip + tessera-project.json manifest), tested without a browser (issue #391, owner
// decision 2026-11-17; format in docs/design/studio-project-io.md section 7).
//
//   node scripts/ci/preview_project_package.mjs
//
// Covered: a package round-trips with every file's time and is byte-deterministic; a plain zip still opens
// with unknown times; a newer version is refused with a message; unknown keys are ignored; a changed size or
// checksum is refused by name; and the stale-generated advice works for a package and stays silent for a
// plain zip.
import assert from 'node:assert/strict';

import { staleGenerated } from '../../designer/web/generated-staleness.js';
import { loadZip } from '../../designer/web/project-io.js';
import {
  MANIFEST_PATH,
  PackageError,
  VERSION,
  applyManifest,
  buildManifest,
  writePackage,
} from '../../designer/web/project-package.js';
import { readZip, writeZip } from '../../designer/web/studio-zip.js';

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

const enc = new TextEncoder();
const file = (path, text, modified) => ({ path, bytes: enc.encode(text), modified });
const PROJECT = [
  file('tessera.json', '{"app":{"entry":"ui/Home.tsx"}}', 1_700_000_000_000),
  file('ui/Home.tsx', 'export function Home() {}', 1_700_000_500_000),
  file('gen/Home.g.hpp', '// generated\nstruct Home {};', 1_700_000_100_000),
];
const manifestOf = (entries) => JSON.parse(new TextDecoder().decode(entries.find((e) => e.path === MANIFEST_PATH).bytes));
const withManifest = (manifest, files = PROJECT) =>
  writeZip([...files.map(({ path, bytes }) => ({ path, bytes })), { path: MANIFEST_PATH, bytes: enc.encode(JSON.stringify(manifest)) }]);
const code = (expected) => (error) => error instanceof PackageError && error.code === expected;

await test('a package round-trips: files, and each time in whole seconds', async () => {
  const { entries } = await loadZip(writePackage(PROJECT));
  assert.deepEqual(entries.map((e) => e.path).sort(), ['gen/Home.g.hpp', 'tessera.json', 'ui/Home.tsx']);
  assert.ok(!entries.some((e) => e.path === MANIFEST_PATH), 'the manifest is not a project file');
  const byPath = Object.fromEntries(entries.map((e) => [e.path, e]));
  assert.equal(byPath['ui/Home.tsx'].lastModified, 1_700_000_500_000);
  assert.equal(byPath['gen/Home.g.hpp'].lastModified, 1_700_000_100_000);
  assert.equal(byPath['ui/Home.tsx'].text, 'export function Home() {}');
});

await test('the same project writes the same bytes, whatever order it is given in', () => {
  assert.deepEqual(writePackage(PROJECT), writePackage([...PROJECT].reverse()));
});

await test('the manifest is compact: version, and one [seconds, size, crc32] row per file', async () => {
  const manifest = manifestOf(await readZip(writePackage(PROJECT)));
  assert.equal(manifest.format, 'tessera-project');
  assert.equal(manifest.version, VERSION);
  assert.deepEqual(Object.keys(manifest.files), ['gen/Home.g.hpp', 'tessera.json', 'ui/Home.tsx']);
  const [seconds, size, crc] = manifest.files['ui/Home.tsx'];
  assert.equal(seconds, 1_700_000_500);
  assert.equal(size, 'export function Home() {}'.length);
  assert.match(crc, /^[0-9a-f]{8}$/);
});

await test('a package inside one wrapper directory still loads with its times', async () => {
  const inner = await readZip(writePackage(PROJECT));
  const wrapped = writeZip(inner.map((e) => ({ path: `app/${e.path}`, bytes: e.bytes })));
  const { entries } = await loadZip(wrapped);
  assert.equal(entries.find((e) => e.path === 'ui/Home.tsx').lastModified, 1_700_000_500_000);
});

await test('a plain zip has no manifest: it opens with unknown times', async () => {
  const plain = writeZip(PROJECT.map(({ path, bytes }) => ({ path, bytes })));
  const { entries } = await loadZip(plain);
  assert.equal(entries.length, 3);
  assert.ok(entries.every((e) => e.lastModified === 0));
});

await test('a newer manifest version is refused with a message naming both versions', () => {
  const bytes = withManifest({ format: 'tessera-project', version: VERSION + 1, files: {} });
  return assert.rejects(loadZip(bytes), (error) => code('version')(error) && /version 2/.test(error.message) && /up to version 1/.test(error.message));
});

await test('unknown keys and extra row items are ignored (forward compatible)', async () => {
  const bytes = withManifest({
    format: 'tessera-project',
    version: 1,
    generator: 'a later studio',
    extras: { anything: true },
    files: { 'ui/Home.tsx': [1_700_000_500, 'export function Home() {}'.length, null, 'future', { x: 1 }] },
  });
  const { entries } = await loadZip(bytes);
  assert.equal(entries.find((e) => e.path === 'ui/Home.tsx').lastModified, 1_700_000_500_000);
  assert.equal(entries.find((e) => e.path === 'tessera.json').lastModified, 0, 'unlisted file: unknown time');
});

await test('a file whose size differs from the manifest is refused by name', () => {
  const tampered = [file('ui/Home.tsx', 'export function Home() {} // edited', 0), PROJECT[0]];
  const bytes = writeZip([
    ...tampered.map(({ path, bytes }) => ({ path, bytes })),
    { path: MANIFEST_PATH, bytes: buildManifest(PROJECT) },
  ]);
  return assert.rejects(loadZip(bytes), (error) => code('size')(error) && /ui\/Home\.tsx/.test(error.message));
});

await test('a same-size change is caught by the checksum; a checksum is optional', async () => {
  const text = 'export function Home() {}';
  const flipped = file('ui/Home.tsx', text.replace('Home', 'Hame'), 0);
  const withHash = { format: 'tessera-project', version: 1, files: { 'ui/Home.tsx': [1, text.length, '00000000'] } };
  await assert.rejects(loadZip(withHash && writeZip([{ path: flipped.path, bytes: flipped.bytes }, { path: MANIFEST_PATH, bytes: enc.encode(JSON.stringify(withHash)) }])), code('hash'));
  const noHash = { format: 'tessera-project', version: 1, files: { 'ui/Home.tsx': [1, text.length] } };
  const { entries } = await loadZip(writeZip([{ path: flipped.path, bytes: flipped.bytes }, { path: MANIFEST_PATH, bytes: enc.encode(JSON.stringify(noHash)) }]));
  assert.equal(entries[0].lastModified, 1000);
});

await test('a manifest that is not JSON, not ours, or malformed is refused', async () => {
  const wrap = (bytes) => writeZip([{ path: 'ui/Home.tsx', bytes: enc.encode('x') }, { path: MANIFEST_PATH, bytes }]);
  await assert.rejects(loadZip(wrap(enc.encode('{not json'))), code('malformed'));
  await assert.rejects(loadZip(wrap(enc.encode('{"format":"other","version":1,"files":{}}'))), code('format'));
  await assert.rejects(loadZip(wrap(enc.encode('{"format":"tessera-project","version":"1","files":{}}'))), code('malformed'));
  await assert.rejects(loadZip(wrap(enc.encode('{"format":"tessera-project","version":1}'))), code('malformed'));
  await assert.rejects(loadZip(wrap(enc.encode('{"format":"tessera-project","version":1,"files":{"ui/Home.tsx":["x",1]}}'))), code('malformed'));
});

await test('the stale-generated advice works for a package and stays silent for a plain zip', async () => {
  const advice = async (bytes) => {
    const { entries } = await loadZip(bytes);
    return staleGenerated(entries.map((e) => ({ path: e.path, kind: e.path.endsWith('.tsx') ? 'source' : 'other', text: e.text, modified: e.lastModified })));
  };
  assert.deepEqual(await advice(writePackage(PROJECT)), ['gen/Home.g.hpp']);
  assert.deepEqual(await advice(writeZip(PROJECT.map(({ path, bytes }) => ({ path, bytes })))), []);
});

await test('applyManifest leaves a plain entry list alone', () => {
  const result = applyManifest([{ path: 'a.tsx', bytes: enc.encode('x') }]);
  assert.equal(result.packaged, false);
  assert.equal(result.entries[0].modified, 0);
});

if (failures > 0) {
  process.stdout.write(`\nproject package: ${failures} case(s) failed\n`);
  process.exit(1);
}
process.stdout.write('\nproject package: all cases passed\n');
