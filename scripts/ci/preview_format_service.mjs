// SPDX-License-Identifier: MIT

// The format service gate (#577, ADR-061 slice 2, ADR-060): drives the format worker through
// docs/specs/studio-services-v1.md with no UI, under plain Node. The worker is built around an engine.
// With no native formatter the engine is a small JS stand-in (protocol behaviour only); when
// TESSERA_FMT_BIN names a built `tessera_fmt`, the same cases also run against the real C++ printer
// through `--edits-json`, and when TESSERA_FMT_WASM names a built `tessera_format.js` they run against the
// Wasm module through the page's own engine source, so the wire shape and the UTF-16 offsets are checked
// end to end.

import { pathToFileURL } from 'node:url';
import { Worker } from 'node:worker_threads';

import { ServiceClient } from '../../designer/web/services/client.js';
import { formatServiceSource, wasmEngineSource } from '../../designer/web/services/format-service.js';
import { toSequentialEdits } from '../../designer/web/services/protocol.js';
import { assert, finish, test } from './mock_test_support.mjs';

const STANDIN_ENGINE = String.raw`
async function tesseraFormat(text, printWidth, indentWidth, kind) {
  if (text.includes('@@slow')) await new Promise((r) => setTimeout(r, 80));
  if (text.includes('@@kind')) return JSON.stringify({ edits: [], diagnostics: [], skipped: [{ code: 'KIND', message: String(kind), line: 1, column: 1 }] });
  if (text.includes('@@refuse')) {
    return JSON.stringify({ edits: [], diagnostics: [{ code: 'TESSERA_FMT_SYNTAX', message: 'refused', line: 1, column: 1 }] });
  }
  const edits = [];
  const re = /[ ]+(?=\n|$)/g;
  let m;
  while ((m = re.exec(text)) !== null) edits.push({ offset: m.index, length: m[0].length, newText: '' });
  return JSON.stringify({ edits, diagnostics: [] });
}`;

const nativeEngine = (bin) => String.raw`
const { execFileSync } = require('node:child_process');
const { mkdtempSync, writeFileSync, rmSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
async function tesseraFormat(text, printWidth, indentWidth, kind) {
  const dir = mkdtempSync(join(tmpdir(), 'fmt-'));
  const name = kind === 1 ? 'in.css' : kind === 2 ? 'tessera.json' : 'in.tsx';
  try {
    writeFileSync(join(dir, name), text, 'utf8');
    return execFileSync(${JSON.stringify(bin)}, ['--recover', '--edits-json', join(dir, name)], { encoding: 'utf8' });
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}`;

function nodeServiceWorker(source) {
  const shim = [
    'const { parentPort } = require("node:worker_threads");',
    'globalThis.postMessage = (m) => parentPort.postMessage(m);',
    'globalThis.addEventListener = (t, f) => parentPort.on("message", (d) => f({ data: d }));',
  ].join('\n');
  const worker = new Worker(`${shim}\n${source}`, { eval: true });
  return {
    post: (m, transfer = []) => worker.postMessage(m, transfer),
    onMessage: (f) => worker.on('message', f),
    onError: (f) => worker.on('error', (e) => f(String(e?.message ?? e))),
    terminate: () => void worker.terminate(),
  };
}

const code = async (promise) => promise.then(() => null, (e) => e.code);
const apply = (text, edits) =>
  toSequentialEdits(edits).reduce((t, e) => t.slice(0, e.offset) + e.newText + t.slice(e.offset + e.length), text);

async function started(engine, text = 'a  \nb\n') {
  const client = new ServiceClient({ name: 'format', createWorker: () => nodeServiceWorker(formatServiceSource(engine)) });
  await client.start();
  await client.setSnapshot([[1, text]]);
  return client;
}

async function commonCases(label, engine, doc) {
  const t = (name, body) => test(`${label}: ${name}`, body);

  await t('the handshake names the format service and its one feature', async () => {
    const client = new ServiceClient({ name: 'format', createWorker: () => nodeServiceWorker(formatServiceSource(engine)) });
    assert.deepEqual(await client.start(), { name: 'format', protocol: 1, features: ['format'] });
    client.dispose();
  });

  await t('format answers an edit list whose application is the formatted text', async () => {
    const client = await started(engine, doc.messy);
    const result = await client.request('format', { file: 1 });
    assert.equal(result.changed, true);
    assert.deepEqual(result.diagnostics, []);
    assert.equal(apply(doc.messy, result.edits), doc.formatted);
    assert.ok(result.edits.every((e) => e.file === 1));
    client.dispose();
  });

  await t('a second format over the formatted text answers no edits (round trip through the client)', async () => {
    const client = await started(engine, doc.messy);
    const first = await client.request('format', { file: 1 });
    await client.applyEdits(toSequentialEdits(first.edits));
    assert.equal(client.files.get(1), doc.formatted);
    const second = await client.request('format', { file: 1 });
    assert.equal(second.changed, false);
    assert.deepEqual(second.edits, []);
    client.dispose();
  });

  await t('a format after a delta composes against the new base version', async () => {
    const client = await started(engine, doc.formatted);
    assert.equal((await client.request('format', { file: 1 })).changed, false);
    await client.applyEdits([{ file: 1, offset: 0, length: 0, newText: doc.prefix }]);
    const result = await client.request('format', { file: 1 });
    assert.equal(apply(doc.prefix + doc.formatted, result.edits), doc.prefixFormatted);
    client.dispose();
  });

  await t('a refused file answers diagnostics and no edits, so the text stays', async () => {
    const client = await started(engine, doc.refused);
    const result = await client.request('format', { file: 1 });
    assert.deepEqual(result.edits, []);
    assert.equal(result.changed, false);
    assert.ok(result.diagnostics.length > 0);
    assert.equal(typeof result.diagnostics[0].code, 'string');
    client.dispose();
  });

  await t('an unknown file id is an error response', async () => {
    const client = await started(engine, doc.messy);
    assert.equal(await code(client.request('format', { file: 9 })), 'TESSERA_FMT_NO_FILE');
    assert.equal(await code(client.request('format', { file: -1 })), 'TESSERA_STUDIO_MALFORMED');
    client.dispose();
  });

  await t('a request before any snapshot is refused', async () => {
    const client = new ServiceClient({ name: 'format', createWorker: () => nodeServiceWorker(formatServiceSource(engine)) });
    await client.start();
    assert.equal(await code(client.request('format', { file: 1 })), 'TESSERA_STUDIO_NO_SNAPSHOT');
    client.dispose();
  });
}

await commonCases('stand-in engine', STANDIN_ENGINE, {
  messy: 'a  \nb \n',
  formatted: 'a\nb\n',
  prefix: 'c  \n',
  prefixFormatted: 'c\na\nb\n',
  refused: '@@refuse\n',
});

await test('format passes the file kind to the engine and answers the skipped regions', async () => {
  const client = await started(STANDIN_ENGINE, '@@kind');
  assert.equal((await client.request('format', { file: 1 })).skipped[0].message, '0');
  assert.equal((await client.request('format', { file: 1, kind: 1 })).skipped[0].message, '1');
  assert.equal((await client.request('format', { file: 1, kind: 2 })).skipped[0].message, '2');
  assert.equal(await code(client.request('format', { file: 1, kind: 3 })), 'TESSERA_STUDIO_MALFORMED');
  assert.equal(await code(client.request('format', { file: 1, kind: 'css' })), 'TESSERA_STUDIO_MALFORMED');
  client.dispose();
});

await test('latest wins: a newer format cancels the older one', async () => {
  const client = await started(STANDIN_ENGINE, 'a  \n@@slow\n');
  const older = code(client.request('format', { file: 1 }));
  const newer = client.request('format', { file: 1 });
  assert.equal(await older, 'TESSERA_STUDIO_CANCELLED');
  assert.equal((await newer).changed, true);
  client.dispose();
});

await test('a stale format answer is dropped when the text moved on, never applied late', async () => {
  const client = await started(STANDIN_ENGINE, 'a  \n@@slow\n');
  const slow = code(client.request('format', { file: 1 }));
  await client.applyEdits([{ file: 1, offset: 0, length: 0, newText: 'x' }]);
  assert.equal(await slow, 'TESSERA_STUDIO_STALE');
  client.dispose();
});

await test('a crashed engine is an error response, and the worker keeps answering', async () => {
  const client = await started('async function tesseraFormat(t) { if (t.includes("boom")) throw new Error("boom"); return JSON.stringify({ edits: [], diagnostics: [] }); }', 'boom');
  assert.equal(await code(client.request('format', { file: 1 })), 'TESSERA_FMT_ENGINE');
  await client.setSnapshot([[1, 'fine']]);
  assert.equal((await client.request('format', { file: 1 })).changed, false);
  client.dispose();
});

const bin = process.env.TESSERA_FMT_BIN;
const wasm = process.env.TESSERA_FMT_WASM;
const realDoc = {
  messy: `const a=  'x';\n`,
  formatted: `const a = "x";\n`,
  prefix: `const  z=1;\n`,
  prefixFormatted: `const z = 1;\nconst a = "x";\n`,
  refused: 'const = ;\n',
};
const emoji = '\u{1F600}';

async function realCases(label, engine) {
  await commonCases(label, engine, realDoc);
  await test(`${label}: offsets are UTF-16 units, so an astral character before the edit counts twice`, async () => {
    const text = `// ${emoji}\nconst a='x';\n`;
    const client = await started(engine, text);
    const result = await client.request('format', { file: 1 });
    assert.equal(apply(text, result.edits), `// ${emoji}\nconst a = "x";\n`);
    assert.ok(result.edits.every((e) => e.offset >= text.indexOf('const')), 'nothing before the change is touched');
    assert.ok(result.edits.length <= 3, 'a minimal list, not the whole file');
    client.dispose();
  });
  await test(`${label}: CSS and tessera.json are formatted by kind`, async () => {
    const css = 'a{color:red}\n';
    const client = await started(engine, css);
    const result = await client.request('format', { file: 1, kind: 1 });
    assert.equal(apply(css, result.edits), 'a {\n  color: red;\n}\n');
    const json = '{"a":1}\n';
    await client.setSnapshot([[1, json]]);
    assert.equal(apply(json, (await client.request('format', { file: 1, kind: 2 })).edits), '{\n  "a": 1\n}\n');
    client.dispose();
  });
  await test(`${label}: a .tsx file with one broken item is formatted around it`, async () => {
    const broken = 'export function Bad() {\n  return <div>oops </span>;\n}\n';
    const text = `const  a=1;\n\n${broken}`;
    const client = await started(engine, text);
    const result = await client.request('format', { file: 1 });
    assert.deepEqual(result.diagnostics, []);
    assert.equal(apply(text, result.edits), `const a = 1;\n\n${broken}`);
    assert.equal(result.skipped.length, 1);
    assert.equal(result.skipped[0].code, 'TESSERA_FMT_SKIPPED');
    client.dispose();
  });
}

if (bin) await realCases('native tessera_fmt', nativeEngine(bin));
else process.stdout.write('skip native tessera_fmt cases (set TESSERA_FMT_BIN to a built tessera_fmt)\n');
// The page's engine: tessera_format.wasm through the page's own wasmEngineSource.
if (wasm) await realCases('tessera_format.wasm', wasmEngineSource(pathToFileURL(wasm).href));
else process.stdout.write('skip wasm cases (set TESSERA_FMT_WASM to a built tessera_format.js)\n');

finish('preview_format_service');
