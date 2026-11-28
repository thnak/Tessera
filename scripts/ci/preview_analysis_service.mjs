// SPDX-License-Identifier: MIT

// The analysis service gate (#568, ADR-060): drives the analysis worker through
// docs/specs/studio-services-v1.md with no UI, under plain Node. The worker is built around an engine.
// With no Wasm module the engine is a small JS stand-in (protocol behaviour only: stale drop, latest
// wins, off-thread parse). When TESSERA_ANALYSIS_WASM names a built tessera_analysis.js the same
// cases run against the C++ library through the page's own engine source, so the wire shape and the
// UTF-16 spans are checked end to end.

import { pathToFileURL } from 'node:url';
import { Worker } from 'node:worker_threads';

import { ServiceClient } from '../../designer/web/services/client.js';
import { analysisServiceSource, wasmEngineSource } from '../../designer/web/services/analysis-service.js';
import { assert, finish, test } from './mock_test_support.mjs';

// The stand-in keeps the project as plain text and reports one diagnostic per line that holds
// "@@error". "@@slow" burns CPU in the worker, which is how the gate measures that a parse does not block
// the thread that owns the page.
const STANDIN_ENGINE = String.raw`
let project = new Map();
function tesseraAnalysisSnapshot(files) { project = new Map(files.map((f) => [f.file, f.text])); }
function tesseraAnalysisEdits(edits) {
  for (const e of edits) {
    if (!project.has(e.file)) return false;
    const t = project.get(e.file);
    project.set(e.file, t.slice(0, e.offset) + e.newText + t.slice(e.offset + e.length));
  }
  return true;
}
function tesseraAnalysisQuery(type, file) {
  const files = [...project.keys()].sort((a, b) => a - b).filter((id) => file < 0 || id === file);
  const burn = [...project.values()].some((t) => t.includes('@@slow'));
  if (burn) { const end = Date.now() + 150; while (Date.now() < end) { /* a long parse */ } }
  return JSON.stringify({ files: files.map((id) => {
    const text = project.get(id);
    const at = text.indexOf('@@error');
    const diagnostics = at < 0 ? [] : [{ code: 'STANDIN', severity: 'error', message: 'e', from: at, to: at + 7 }];
    return type === 'symbols' ? { file: id, indexed: true, symbols: [], references: [] } : { file: id, diagnostics };
  }) });
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
const make = (engine) => new ServiceClient({ name: 'analysis', createWorker: () => nodeServiceWorker(analysisServiceSource(engine)) });

async function started(engine, files) {
  const client = make(engine);
  await client.start();
  await client.setSnapshot(files);
  return client;
}

async function protocolCases(label, engine, sample) {
  const t = (name, body) => test(`${label}: ${name}`, body);

  await t('the handshake names the analysis service and its features', async () => {
    const client = make(engine);
    assert.deepEqual(await client.start(), { name: 'analysis', protocol: 1, features: ['symbols', 'diagnostics'] });
    assert.equal(client.supports('diagnostics'), true);
    client.dispose();
  });

  await t('diagnostics for the whole project: one entry per file, in file id order', async () => {
    const client = await started(engine, sample.files);
    const result = await client.request('diagnostics');
    assert.deepEqual(result.files.map((f) => f.file), sample.files.map((f) => f[0]).sort((a, b) => a - b));
    client.dispose();
  });

  await t('an error in an imported file is reported against that file', async () => {
    const client = await started(engine, sample.files);
    const result = await client.request('diagnostics');
    const byFile = new Map(result.files.map((f) => [f.file, f.diagnostics]));
    assert.equal(byFile.get(sample.brokenFile).length > 0, true);
    assert.equal(byFile.get(sample.cleanFile).length, 0);
    client.dispose();
  });

  await t('a single-file request answers only that file; an unknown id is an error', async () => {
    const client = await started(engine, sample.files);
    const result = await client.request('diagnostics', { file: sample.brokenFile });
    assert.deepEqual(result.files.map((f) => f.file), [sample.brokenFile]);
    assert.equal(await code(client.request('diagnostics', { file: 99 })), 'TESSERA_ANALYSIS_NO_FILE');
    assert.equal(await code(client.request('diagnostics', { file: -1 })), 'TESSERA_STUDIO_MALFORMED');
    client.dispose();
  });

  await t('a delta that repairs the file clears its diagnostics (incremental)', async () => {
    const client = await started(engine, sample.files);
    await client.applyEdits(sample.repair);
    const result = await client.request('diagnostics');
    assert.equal(result.files.every((f) => f.diagnostics.length === 0), true);
    client.dispose();
  });

  await t('symbols are served by the same worker', async () => {
    const client = await started(engine, sample.files);
    const result = await client.request('symbols', { file: sample.cleanFile });
    assert.equal(result.files.length, 1);
    assert.equal(Array.isArray(result.files[0].symbols), true);
    client.dispose();
  });

  await t('a delta on a file the cache does not hold asks for a snapshot', async () => {
    const client = await started(engine, sample.files);
    assert.equal(await code(client.applyEdits([{ file: 77, offset: 0, length: 0, newText: 'x' }])), 'TESSERA_STUDIO_VERSION');
    client.dispose();
  });

  await t('a request before any snapshot is refused, and an unknown type too', async () => {
    const client = make(engine);
    await client.start();
    assert.equal(await code(client.request('diagnostics')), 'TESSERA_STUDIO_NO_SNAPSHOT');
    await client.setSnapshot(sample.files);
    assert.equal(await code(client.request('hover', {})), 'TESSERA_STUDIO_UNKNOWN_TYPE');
    client.dispose();
  });
}

const standinSample = {
  files: [[0, 'ok\n', 'App.tsx'], [1, 'bad @@error\n', 'Card.tsx']],
  brokenFile: 1,
  cleanFile: 0,
  repair: [{ file: 1, offset: 4, length: 7, newText: '' }],
};
await protocolCases('stand-in engine', STANDIN_ENGINE, standinSample);

await test('latest wins: a newer diagnostics request cancels the older one', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@slow\n', 'a.tsx']]);
  const older = code(client.request('diagnostics'));
  const newer = client.request('diagnostics');
  assert.equal(await older, 'TESSERA_STUDIO_CANCELLED');
  assert.equal((await newer).files.length, 1);
  client.dispose();
});

await test('an edit while a request is in flight drops the stale answer, never applies it late', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@slow\n', 'a.tsx']]);
  const slow = code(client.request('diagnostics'));
  await client.applyEdits([{ file: 0, offset: 0, length: 0, newText: 'x' }]);
  assert.equal(await slow, 'TESSERA_STUDIO_STALE');
  // The re-ask answers the new text.
  assert.equal((await client.request('diagnostics')).files.length, 1);
  client.dispose();
});

await test('typing is not blocked: the page thread keeps ticking while the worker parses', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@slow\n', 'a.tsx']]);
  let worst = 0;
  let last = performance.now();
  const timer = setInterval(() => {
    const now = performance.now();
    worst = Math.max(worst, now - last);
    last = now;
  }, 5);
  const began = performance.now();
  await client.request('diagnostics');
  const took = performance.now() - began;
  clearInterval(timer);
  assert.equal(took >= 140, true, `the parse took ${took.toFixed(0)} ms in the worker`);
  // The worker burned 150 ms; the page thread's longest gap stays far below that.
  assert.equal(worst < 100, true, `longest page-thread gap ${worst.toFixed(0)} ms`);
  client.dispose();
});

await test('a crashed engine is an error response, and the worker keeps answering', async () => {
  const engine = 'function tesseraAnalysisSnapshot() {} function tesseraAnalysisEdits() { return true; } function tesseraAnalysisQuery(t) { if (t === "symbols") throw new Error("boom"); return JSON.stringify({ files: [] }); }';
  const client = await started(engine, [[0, 'x', 'a.tsx']]);
  assert.equal(await code(client.request('symbols')), 'TESSERA_ANALYSIS_ENGINE');
  assert.deepEqual(await client.request('diagnostics'), { files: [] });
  client.dispose();
});

// The C++ library through the page's engine, when a Wasm module is built.
const wasm = process.env.TESSERA_ANALYSIS_WASM;
const emoji = '\u{1F600}';
const app = 'import { Card } from "./Card";\nimport { Gone } from "./Gone";\nexport function App() { return <Card />; }\n';
const realSample = {
  files: [[0, app, 'App.tsx'], [1, 'export function Card( {\n', 'Card.tsx'], [2, 'export function Dup() { return <p />; }\n', 'Dup.tsx']],
  brokenFile: 1,
  cleanFile: 2,
  repair: [{ file: 1, offset: 0, length: 23, newText: 'export function Card() { return <p />; }' },
    { file: 0, offset: 31, length: 31, newText: '' }],
};

if (wasm) {
  const engine = wasmEngineSource(pathToFileURL(wasm).href);
  // App.tsx also reports its missing import, so the clean-file expectation moves to Dup.tsx (id 2).
  await protocolCases('tessera_analysis.wasm', engine, realSample);

  await test('tessera_analysis.wasm: project findings and UTF-16 spans', async () => {
    const text = `// ${emoji}\nimport { Gone } from "./Gone";\nimport { Card } from "./Card";\nexport function Card() { return <p />; }\n`;
    const client = await started(engine, [[0, text, 'a.tsx'], [1, 'export function Card() { return <b />; }\n', 'b.tsx']]);
    const result = await client.request('diagnostics');
    const codes = result.files[0].diagnostics.map((d) => d.code);
    assert.equal(codes.includes('TESSERA_ANALYSIS_IMPORT_FILE'), true);
    assert.equal(codes.includes('TESSERA_ANALYSIS_DUPLICATE_COMPONENT'), true);
    const gone = result.files[0].diagnostics.find((d) => d.code === 'TESSERA_ANALYSIS_IMPORT_FILE');
    assert.equal(text.slice(gone.from, gone.to), 'Gone', 'the span is in UTF-16 units: the emoji counts twice');
    const dup = result.files[1].diagnostics[0];
    assert.equal(dup.related.file, 0);
    client.dispose();
  });

  await test('tessera_analysis.wasm: the answer is deterministic across snapshot orders', async () => {
    const a = await started(engine, realSample.files);
    const b = await started(engine, [...realSample.files].reverse());
    assert.deepEqual(await a.request('diagnostics'), await b.request('diagnostics'));
    a.dispose();
    b.dispose();
  });
} else {
  process.stdout.write('skip wasm cases (set TESSERA_ANALYSIS_WASM to a built tessera_analysis.js)\n');
}

finish('preview_analysis_service');
