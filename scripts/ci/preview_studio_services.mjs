// SPDX-License-Identifier: MIT

// The no-UI protocol harness for the studio services (#567, ADR-060): drives a service worker through
// docs/specs/studio-services-v1.md with no React, DOM or CodeMirror. Runs under plain Node.

import { Worker } from 'node:worker_threads';

import { ServiceClient } from '../../designer/web/services/client.js';
import { validateMessage } from '../../designer/web/services/protocol.js';
import { STUB_SERVICE_SOURCE } from '../../designer/web/services/stub-service.js';
import { assert, finish, test } from './mock_test_support.mjs';

// Node stand-in for a page worker; unlike the mock gate's it honours the transfer list.
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

async function started() {
  const client = new ServiceClient({ name: 'stub', createWorker: () => nodeServiceWorker(STUB_SERVICE_SOURCE) });
  await client.start();
  await client.setSnapshot([[1, 'hello world']]);
  return client;
}
const code = async (promise) => promise.then(() => null, (e) => e.code);

await test('capability handshake names the service, protocol and features', async () => {
  const client = new ServiceClient({ name: 'stub', createWorker: () => nodeServiceWorker(STUB_SERVICE_SOURCE) });
  const caps = await client.start();
  assert.deepEqual(caps, { name: 'stub', protocol: 1, features: ['echo', 'slow'] });
  assert.equal(client.supports('echo'), true);
  assert.equal(client.supports('rename'), false);
  client.dispose();
});

await test('a versioned request is answered against the snapshot', async () => {
  const client = await started();
  assert.deepEqual(await client.request('echo', { file: 1 }), { length: 11 });
  await client.applyEdits([{ file: 1, offset: 5, length: 6, newText: '!' }]);
  assert.deepEqual(await client.request('echo', { file: 1 }), { length: 6 });
  client.dispose();
});

await test('an unknown request type is an error response, not a crash', async () => {
  const client = await started();
  assert.equal(await code(client.request('rename', {})), 'TESSERA_STUDIO_UNKNOWN_TYPE');
  assert.deepEqual(await client.request('echo', { file: 1 }), { length: 11 });
  client.dispose();
});

await test('latest wins: a newer request of the same type cancels the older one', async () => {
  const client = await started();
  const older = code(client.request('slow', { delayMs: 200 }));
  const newer = client.request('slow', { delayMs: 10 });
  assert.equal(await older, 'TESSERA_STUDIO_CANCELLED');
  assert.deepEqual(await newer, { done: true });
  client.dispose();
});

await test('a stale answer is dropped when the text moved on', async () => {
  const client = await started();
  const slow = code(client.request('slow', { delayMs: 50 }));
  await client.applyEdits([{ file: 1, offset: 0, length: 0, newText: 'x' }]);
  assert.equal(await slow, 'TESSERA_STUDIO_STALE');
  assert.deepEqual(await client.request('slow', { delayMs: 1 }), { done: true });
  client.dispose();
});

await test('a request before any snapshot is an error, and restart rebuilds from the next snapshot', async () => {
  const client = await started();
  await client.applyEdits([{ file: 1, offset: 0, length: 5, newText: 'bye' }]);
  const caps = await client.restart();
  assert.equal(caps.name, 'stub');
  assert.deepEqual(await client.request('echo', { file: 1 }), { length: 9 });
  const fresh = new ServiceClient({ name: 'stub', createWorker: () => nodeServiceWorker(STUB_SERVICE_SOURCE) });
  await fresh.start();
  assert.equal(await code(fresh.request('echo', { file: 1 })), 'TESSERA_STUDIO_NO_SNAPSHOT');
  client.dispose();
  fresh.dispose();
});

await test('a delta against the wrong base version is refused by the worker', async () => {
  const w = nodeServiceWorker(STUB_SERVICE_SOURCE);
  const answers = [];
  w.onMessage((m) => answers.push(m));
  const send = (type, id, version, payload) => w.post({ v: 1, kind: 'request', service: 'stub', type, id, version, payload });
  send('snapshot', 1, 1, { files: [{ file: 1, bytes: new TextEncoder().encode('a').buffer }] });
  send('delta', 2, 9, { baseVersion: 7, edits: [] });
  while (answers.length < 2) await new Promise((r) => setTimeout(r, 5));
  assert.equal(answers[1].payload.error.code, 'TESSERA_STUDIO_VERSION');
  w.terminate();
});

await test('schema check rejects functions and DOM-like values', async () => {
  const ok = { v: 1, kind: 'request', service: 'stub', type: 'echo', id: 1, version: 1, payload: { file: 1 } };
  assert.deepEqual(validateMessage(ok), []);
  const withFn = { ...ok, payload: { file: 1, cb: () => 1 } };
  assert.match(validateMessage(withFn).join(), /function is not plain data/);
  class FakeElement {}
  assert.match(validateMessage({ ...ok, payload: { node: new FakeElement() } }).join(), /FakeElement is not plain data/);
  assert.match(validateMessage({ ...ok, payload: { m: new Map() } }).join(), /Map is not plain data/);
  const cyc = { file: 1 }; cyc.self = cyc;
  assert.match(validateMessage({ ...ok, payload: cyc }).join(), /cycle/);
  assert.notEqual(validateMessage({ ...ok, v: 2 }).length, 0);
  assert.notEqual(validateMessage({ ...ok, id: undefined }).length, 0);
  assert.notEqual(validateMessage({ ...ok, type: 'snapshot', payload: { files: [{ file: 1, bytes: 'x' }] } }).length, 0);
  const client = await started();
  await assert.rejects(client.request('echo', { file: 1, cb: () => 1 }), { code: 'TESSERA_STUDIO_MALFORMED' });
  client.dispose();
});

finish('studio services protocol');
