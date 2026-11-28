// SPDX-License-Identifier: MIT

// The analysis service gate (#568, ADR-060): drives the analysis worker through
// docs/specs/studio-services-v1.md with no UI, under plain Node. The worker is built around an engine.
// With no Wasm module the engine is a small JS stand-in (protocol behaviour only: stale drop, latest
// wins, off-thread parse). When TESSERA_ANALYSIS_WASM names a built tessera_analysis.js the same
// cases run against the C++ library through the page's own engine source, so the wire shape and the
// UTF-16 spans are checked end to end.

import { readFileSync, readdirSync } from 'node:fs';
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
function tesseraAnalysisQuery(type, file, offset, flag, text) {
  if (offset !== undefined) {
    if (project.get(file).includes('@@slow')) { const end = Date.now() + 150; while (Date.now() < end) { /* a slow answer */ } }
    return JSON.stringify({ echo: type === 'rename' ? [type, file, offset, flag, text] : [type, file, offset, flag] });
  }
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
    assert.deepEqual(await client.start(), { name: 'analysis', protocol: 1, features: ['symbols', 'diagnostics', 'definition', 'references', 'hover', 'completion', 'prepareRename', 'rename'] });
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

  await t('position requests validate file and offset before the engine is asked', async () => {
    const client = await started(engine, sample.files);
    for (const type of ['definition', 'references', 'hover', 'completion', 'prepareRename', 'rename']) {
      assert.equal(await code(client.request(type, { offset: 0 })), 'TESSERA_STUDIO_MALFORMED', type + ' needs a file');
      assert.equal(await code(client.request(type, { file: sample.cleanFile })), 'TESSERA_STUDIO_MALFORMED', type + ' needs an offset');
      assert.equal(await code(client.request(type, { file: sample.cleanFile, offset: -1 })), 'TESSERA_STUDIO_MALFORMED');
      assert.equal(await code(client.request(type, { file: sample.cleanFile, offset: 1.5 })), 'TESSERA_STUDIO_MALFORMED');
      assert.equal(await code(client.request(type, { file: 99, offset: 0 })), 'TESSERA_ANALYSIS_NO_FILE');
    }
    assert.equal(await code(client.request('references', { file: sample.cleanFile, offset: 0, includeDeclaration: 'yes' })), 'TESSERA_STUDIO_MALFORMED');
    client.dispose();
  });

  await t('a request before any snapshot is refused, and an unknown type too', async () => {
    const client = make(engine);
    await client.start();
    assert.equal(await code(client.request('diagnostics')), 'TESSERA_STUDIO_NO_SNAPSHOT');
    await client.setSnapshot(sample.files);
    assert.equal(await code(client.request('bogus', {})), 'TESSERA_STUDIO_UNKNOWN_TYPE');
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

await test('stand-in engine: position requests reach the engine with file, offset and includeDeclaration', async () => {
  const client = await started(STANDIN_ENGINE, [[0, 'x\n', 'a.tsx']]);
  assert.deepEqual(await client.request('hover', { file: 0, offset: 1 }), { echo: ['hover', 0, 1, 1] });
  assert.deepEqual(await client.request('references', { file: 0, offset: 2, includeDeclaration: false }), { echo: ['references', 0, 2, 0] });
  client.dispose();
});

await test('stand-in engine: completion, prepareRename and rename reach the engine; rename needs a string newName', async () => {
  const client = await started(STANDIN_ENGINE, [[0, 'x\n', 'a.tsx']]);
  assert.deepEqual(await client.request('completion', { file: 0, offset: 1 }), { echo: ['completion', 0, 1, 1] });
  assert.deepEqual(await client.request('prepareRename', { file: 0, offset: 0 }), { echo: ['prepareRename', 0, 0, 1] });
  assert.deepEqual(await client.request('rename', { file: 0, offset: 0, newName: 'Tile' }), { echo: ['rename', 0, 0, 1, 'Tile'] });
  assert.equal(await code(client.request('rename', { file: 0, offset: 0 })), 'TESSERA_STUDIO_MALFORMED');
  assert.equal(await code(client.request('rename', { file: 0, offset: 0, newName: 3 })), 'TESSERA_STUDIO_MALFORMED');
  client.dispose();
});

await test('latest wins: a newer completion request cancels the older one', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@slow\n', 'a.tsx']]);
  const older = code(client.request('completion', { file: 0, offset: 1 }));
  const newer = client.request('completion', { file: 0, offset: 2 });
  assert.equal(await older, 'TESSERA_STUDIO_CANCELLED');
  assert.deepEqual(await newer, { echo: ['completion', 0, 2, 1] });
  client.dispose();
});

await test('stand-in engine: a hover answered for an older version is dropped as stale', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@slow\n', 'a.tsx']]);
  const slow = code(client.request('hover', { file: 0, offset: 0 }));
  await client.applyEdits([{ file: 0, offset: 0, length: 0, newText: 'x' }]);
  assert.equal(await slow, 'TESSERA_STUDIO_STALE');
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

  await test('tessera_analysis.wasm: definition, references and hover across files, in UTF-16 offsets', async () => {
    const card = '/** A card. */\nexport function Card({ title }) { return <p>{title}</p>; }\n';
    const app = `// ${emoji}\nimport { Card } from "./Card";\nexport function App() { return <Card title="a" />; }\n`;
    const client = await started(engine, [[0, app, 'App.tsx'], [1, card, 'Card.tsx']]);
    const tag = app.indexOf('<Card') + 2;
    const def = await client.request('definition', { file: 0, offset: tag });
    assert.equal(def.definitions.length, 1);
    assert.equal(def.definitions[0].file, 1);
    assert.equal(card.slice(def.definitions[0].from, def.definitions[0].to), 'Card');
    const prop = await client.request('definition', { file: 0, offset: app.indexOf('title=') });
    assert.equal(card.slice(prop.definitions[0].from, prop.definitions[0].to), 'title');
    const refs = await client.request('references', { file: 1, offset: card.indexOf('Card(') + 1 });
    assert.equal(refs.symbol, 'f1:Card');
    assert.deepEqual(refs.references.map((r) => [r.file, app.slice(r.from, r.to) === 'Card' || card.slice(r.from, r.to) === 'Card']), [[0, true], [0, true], [1, true]]);
    const noDecl = await client.request('references', { file: 1, offset: card.indexOf('Card(') + 1, includeDeclaration: false });
    assert.equal(noDecl.references.length, 1);
    const tip = (await client.request('hover', { file: 0, offset: tag })).hover;
    assert.equal(tip.kind, 'component');
    assert.equal(tip.doc, 'A card.');
    assert.equal(tip.declaration.file, 1);
    assert.equal(app.slice(tip.from, tip.to), 'Card', 'the hover range is in UTF-16 units: the emoji counts twice');
    assert.equal((await client.request('hover', { file: 0, offset: 1 })).hover, null);
    // An edit moves every answer with it.
    await client.applyEdits([{ file: 1, offset: 0, length: 0, newText: '// pad\n' }]);
    const moved = await client.request('definition', { file: 0, offset: tag });
    assert.equal(moved.definitions[0].from, def.definitions[0].from + 7);
    client.dispose();
  });

  await test('tessera_analysis.wasm: the answer is deterministic across snapshot orders', async () => {
    const a = await started(engine, realSample.files);
    const b = await started(engine, [...realSample.files].reverse());
    assert.deepEqual(await a.request('diagnostics'), await b.request('diagnostics'));
    a.dispose();
    b.dispose();
  });

  await test('tessera_analysis.wasm: completions per context, in UTF-16 ranges', async () => {
    const card = 'export function Card({ title, count }) { return <p>{title}</p>; }\n';
    const css = '.card { color: red; }\n.wide { margin: 1px; }\n';
    const app = `// ${emoji}\nimport { Card } from "./Card";\nimport styles from "./App.module.css";\nexport function App() {\n  return <Card ti className={styles.w} />;\n}\n`;
    const client = await started(engine, [[0, app, 'App.tsx'], [1, card, 'Card.tsx'], [2, css, 'App.module.css']]);
    const names = (r) => r.items.map((i) => i.label);
    const attr = await client.request('completion', { file: 0, offset: app.indexOf(' ti ') + 3 });
    assert.equal(attr.context, 'attribute');
    assert.deepEqual(names(attr), ['title']);
    assert.equal(app.slice(attr.from, attr.to), 'ti', 'the range is in UTF-16 units: the emoji counts twice');
    const cls = await client.request('completion', { file: 0, offset: app.indexOf('styles.w') + 8 });
    assert.deepEqual(names(cls), ['wide']);
    const imp = await client.request('completion', { file: 0, offset: app.indexOf('"./Card"') + 3 });
    assert.equal(imp.context, 'importPath');
    assert.deepEqual(names(imp), ['./App.module.css', './Card']);
    const tag = await client.request('completion', { file: 0, offset: app.indexOf('<Card ti') + 1 });
    assert.equal(tag.context, 'tag');
    assert.deepEqual(names(tag), ['App', 'Card']);
    client.dispose();
  });

  await test('tessera_analysis.wasm: prepareRename and rename return multi-file UTF-16 edits, or a refusal', async () => {
    const card = 'export function Card({ title }) { return <p>{title}</p>; }\n';
    const app = `// ${emoji}\nimport { Card } from "./Card";\nexport function App() { return <Card title="a" />; }\n`;
    const client = await started(engine, [[0, app, 'App.tsx'], [1, card, 'Card.tsx']]);
    const tag = app.indexOf('<Card') + 2;
    const prep = (await client.request('prepareRename', { file: 0, offset: tag })).prepare;
    assert.equal(prep.placeholder, 'Card');
    assert.equal(app.slice(prep.from, prep.to), 'Card');
    const before = await client.request('diagnostics');
    const result = await client.request('rename', { file: 0, offset: tag, newName: 'Tile' });
    assert.equal(result.refusal, undefined);
    assert.deepEqual([...new Set(result.edits.map((e) => e.file))], [0, 1]);
    // The edits are valid delta edits: send them back and ask again.
    await client.applyEdits(result.edits);
    const moved = await client.request('definition', { file: 0, offset: tag });
    assert.equal(moved.definitions[0].file, 1);
    assert.deepEqual((await client.request('diagnostics')).files, before.files, 'renaming adds no diagnostic');
    const refused = await client.request('rename', { file: 0, offset: tag, newName: 'App' });
    assert.deepEqual(refused.edits, []);
    assert.equal(refused.refusal.code, 'TESSERA_ANALYSIS_RENAME_COLLISION');
    const none = await client.request('prepareRename', { file: 0, offset: 1 });
    assert.equal(none.prepare, null);
    assert.equal(none.refusal.code, 'TESSERA_ANALYSIS_RENAME_NO_NAME');
    client.dispose();
  });

  await test('tessera_analysis.wasm: a completion over the gallery sample stays inside the 100 ms budget', async () => {
    const dir = new URL('../../samples/gallery/', import.meta.url);
    const names = readdirSync(dir).filter((n) => /\.(tsx|css)$/.test(n)).sort();
    const files = names.map((n, i) => [i, readFileSync(new URL(n, dir), 'utf8'), n]);
    const gallery = files.findIndex((f) => f[2] === 'Gallery.tsx');
    const text = files[gallery][1];
    const at = text.search(/return \(?\s*<[A-Z]/);
    assert.equal(at >= 0, true, 'the sample has a JSX component tag');
    const caret = text.indexOf('<', at) + 1;
    const client = await started(engine, files);
    await client.request('completion', { file: gallery, offset: caret });
    const times = [];
    for (let i = 0; i < 30; i++) {
      const began = performance.now();
      const answer = await client.request('completion', { file: gallery, offset: caret });
      times.push(performance.now() - began);
      assert.equal(answer.context, 'tag');
    }
    times.sort((a, b) => a - b);
    const p95 = times[Math.floor(times.length * 0.95)];
    process.stdout.write(`completion over samples/gallery (${files.length} files): median ${times[15].toFixed(1)} ms, p95 ${p95.toFixed(1)} ms\n`);
    assert.equal(p95 < 100, true, `p95 ${p95.toFixed(1)} ms`);
    client.dispose();
  });
} else {
  process.stdout.write('skip wasm cases (set TESSERA_ANALYSIS_WASM to a built tessera_analysis.js)\n');
}

finish('preview_analysis_service');
