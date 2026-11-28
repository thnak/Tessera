// SPDX-License-Identifier: MIT

// The compile service gate (#569, ADR-060): drives the compile worker, the page's diagnostics view behind
// the services protocol, through docs/specs/studio-services-v1.md with no UI, under plain Node. The worker is
// built around an engine. With no Wasm module the engine is a small JS stand-in (protocol behaviour: the
// project the worker stages, latest wins, stale drop, off-thread compile, a killed worker restarted from the
// next snapshot). When TESSERA_DIAGNOSTICS_WASM names a built tessera_diagnostics.js the same worker runs the
// real compiler through the page's own engine source.

import { pathToFileURL } from 'node:url';

import { ServiceClient } from '../../designer/web/services/client.js';
import { compileServiceSource, wasmEngineSource } from '../../designer/web/services/compile-service.js';
import { assert, finish, nodeWorker, test } from './mock_test_support.mjs';

// The stand-in answers a diagnostics document with one error per source line holding "@@error", and echoes
// what it was staged with. "@@slow" burns CPU in the worker; "@@die" makes the worker throw outside a request.
const STANDIN_ENGINE = String.raw`
function tesseraCompile(project) {
  const burn = project.sources.some((s) => s.text.includes('@@slow'));
  if (burn) { const end = Date.now() + 150; while (Date.now() < end) { /* a long compile */ } }
  if (project.sources.some((s) => s.text.includes('@@throw'))) throw new Error('boom');
  if (project.sources.some((s) => s.text.includes('@@die'))) setTimeout(() => { throw new Error('killed'); }, 0);
  const diagnostics = [];
  for (const s of project.sources) {
    const at = s.text.indexOf('@@error');
    if (at >= 0) diagnostics.push({ code: 'STANDIN', severity: 'error', message: 'e', span: { file: s.path, line: 1, column: 1, offset: at, length: 7 } });
  }
  return JSON.stringify({ diagnostics, staged: { entry: project.entry, sources: project.sources.map((s) => s.path), stylesheets: project.stylesheets.map((s) => s.path), services: project.servicesManifest } });
}`;

const code = async (promise) => promise.then(() => null, (e) => e.code);
const make = (engine) => new ServiceClient({ name: 'compile', createWorker: () => nodeWorker(compileServiceSource(engine)) });

async function started(engine, files) {
  const client = make(engine);
  await client.start();
  await client.setSnapshot(files);
  return client;
}

async function protocolCases(label, engine, sample) {
  const t = (name, body) => test(`${label}: ${name}`, body);

  await t('the handshake names the compile service and its feature', async () => {
    const client = make(engine);
    assert.deepEqual(await client.start(), { name: 'compile', protocol: 1, features: ['diagnostics'] });
    assert.equal(client.supports('diagnostics'), true);
    client.dispose();
  });

  await t('diagnostics returns the compiler document for the snapshot and the entry', async () => {
    const client = await started(engine, sample.files);
    const { document } = await client.request('diagnostics', { entry: sample.entry });
    assert.equal(Array.isArray(document.diagnostics), true);
    assert.equal(document.diagnostics.length > 0, true);
    client.dispose();
  });

  await t('an edit that repairs the file clears its diagnostics (incremental delta)', async () => {
    const client = await started(engine, sample.files);
    await client.applyEdits(sample.repair);
    const { document } = await client.request('diagnostics', { entry: sample.entry });
    assert.equal(document.diagnostics.length, 0);
    client.dispose();
  });

  await t('a request before any snapshot is refused, and an unknown type too', async () => {
    const client = make(engine);
    await client.start();
    assert.equal(await code(client.request('diagnostics', {})), 'TESSERA_STUDIO_NO_SNAPSHOT');
    await client.setSnapshot(sample.files);
    assert.equal(await code(client.request('compile', {})), 'TESSERA_STUDIO_UNKNOWN_TYPE');
    assert.equal(await code(client.request('diagnostics', { entry: 7 })), 'TESSERA_STUDIO_MALFORMED');
    client.dispose();
  });
}

const standinSample = {
  files: [[0, 'ok\n', 'ui/App.tsx'], [1, 'bad @@error\n', 'ui/Card.tsx']],
  entry: 'ui/App.tsx',
  repair: [{ file: 1, offset: 4, length: 7, newText: '' }],
};
await protocolCases('stand-in engine', STANDIN_ENGINE, standinSample);

await test('the worker stages sources and stylesheets by path and passes the entry and manifest through', async () => {
  const client = await started(STANDIN_ENGINE, [[1, '.a{}', 'ui/b.css'], [0, 'x', 'ui/App.tsx']]);
  const { document } = await client.request('diagnostics', { entry: 'ui/App.tsx', servicesManifest: '{"services":[]}' });
  assert.deepEqual(document.staged, { entry: 'ui/App.tsx', sources: ['ui/App.tsx'], stylesheets: ['ui/b.css'], services: '{"services":[]}' });
  const none = (await client.request('diagnostics', {})).document.staged;
  assert.deepEqual([none.entry, none.services], [null, null], 'no entry and no manifest are staged as none');
  client.dispose();
});

await test('latest wins: a newer diagnostics request cancels the older one', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@slow\n', 'a.tsx']]);
  const older = code(client.request('diagnostics', {}));
  const newer = client.request('diagnostics', {});
  assert.equal(await older, 'TESSERA_STUDIO_CANCELLED');
  assert.equal(Array.isArray((await newer).document.diagnostics), true);
  client.dispose();
});

await test('an edit while a compile is in flight drops the stale answer, never applies it late', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@slow\n', 'a.tsx']]);
  const slow = code(client.request('diagnostics', {}));
  await client.applyEdits([{ file: 0, offset: 0, length: 0, newText: 'x' }]);
  assert.equal(await slow, 'TESSERA_STUDIO_STALE');
  client.dispose();
});

await test('typing is not blocked: the page thread keeps ticking while the worker compiles', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@slow\n', 'a.tsx']]);
  let worst = 0;
  let last = performance.now();
  const timer = setInterval(() => {
    const now = performance.now();
    worst = Math.max(worst, now - last);
    last = now;
  }, 5);
  const began = performance.now();
  await client.request('diagnostics', {});
  const took = performance.now() - began;
  clearInterval(timer);
  assert.equal(took >= 140, true, `the compile took ${took.toFixed(0)} ms in the worker`);
  assert.equal(worst < 100, true, `longest page-thread gap ${worst.toFixed(0)} ms`);
  client.dispose();
});

await test('a throwing engine is an error response, and the worker keeps answering', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@throw', 'a.tsx']]);
  assert.equal(await code(client.request('diagnostics', {})), 'TESSERA_COMPILE_ENGINE');
  await client.applyEdits([{ file: 0, offset: 0, length: 7, newText: 'fine' }]);
  assert.equal(Array.isArray((await client.request('diagnostics', {})).document.diagnostics), true);
  client.dispose();
});

await test('a killed worker is restarted from the next snapshot: the open request fails cancelled, the next one answers', async () => {
  const client = await started(STANDIN_ENGINE, [[0, '@@die @@error', 'a.tsx']]);
  const open = code(client.request('diagnostics', {}));
  // The worker answers once, then its timer throws and it dies; whichever the page sees, the next ask recovers.
  await open;
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.notEqual(client.failure, null, 'the page learned the worker died');
  assert.equal(await client.recover(), true);
  assert.equal(client.failure, null);
  await client.applyEdits([{ file: 0, offset: 0, length: 5, newText: 'ok' }]);
  const { document } = await client.request('diagnostics', {});
  assert.equal(document.diagnostics.length, 1, 'the restarted worker rebuilt the project from the page text');
  assert.equal(await client.recover(), false, 'a healthy worker is left alone');
  client.dispose();
});

// The compiler through the page's engine, when a Wasm module is built.
const wasm = process.env.TESSERA_DIAGNOSTICS_WASM;
if (wasm) {
  const engine = wasmEngineSource(pathToFileURL(wasm).href);
  const emoji = '\u{1F600}';
  const bad = 'export function Bad() { const [n] = useState<number>(0); return <View />; }';
  await protocolCases('tessera_diagnostics.wasm', engine, {
    files: [[0, 'export function App() { return <View><Text>a</Text></View>; }\n', 'ui/App.tsx'], [1, bad + '\n', 'ui/Bad.tsx']],
    entry: 'ui/App.tsx',
    repair: [{ file: 1, offset: 0, length: bad.length, newText: 'export function Bad() { return <View />; }' }],
  });
  await test('tessera_diagnostics.wasm: the document is the one the page rendered before the worker', async () => {
    const client = await started(engine, [[0, `// ${emoji}\nexport function App() { const [n] = useState<number>(0); return <View />; }\n`, 'ui/App.tsx']]);
    const { document } = await client.request('diagnostics', { entry: 'ui/App.tsx' });
    const bare = document.diagnostics.find((d) => d.code === 'TESSERA_JSX_TYPE_NUMBER');
    assert.equal(bare !== undefined && bare.span.file === 'ui/App.tsx', true);
    client.dispose();
  });
} else {
  process.stdout.write('skip wasm cases (set TESSERA_DIAGNOSTICS_WASM to a built tessera_diagnostics.js)\n');
}

finish('preview_compile_service');
