// SPDX-License-Identifier: MIT
//
// Shared pieces of the two mock gates (`preview_mock_host.mjs`, `preview_mock_sandbox.mjs`): the tiny
// test runner, the Node stand-in for the page's blob worker, a manifest and two sample mocks.

import assert from 'node:assert/strict';
import { Worker } from 'node:worker_threads';

import { createMockHost } from '../../designer/web/mock/host.js';
import { compileMock } from '../../designer/web/mock/transpile.js';

export { assert };

let failures = 0;
export async function test(name, body) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.stack ? error.stack : error}\n`);
  }
}

/** Prints the verdict and sets the exit code; call once at the end of a gate. */
export function finish(label) {
  process.stdout.write(failures === 0 ? `\n${label}: all cases passed\n` : `\n${label}: ${failures} failed\n`);
  process.exit(failures > 0 ? 1 : 0);
}

// --- the Node stand-in for the page's blob worker --------------------------------------------------

export function nodeWorker(source) {
  // The shim gives the worker the three browser-shaped things the prelude captures. It runs before the
  // prelude, exactly like the browser's own globals exist before the worker script does.
  const shim = [
    'const { parentPort } = require("node:worker_threads");',
    'globalThis.postMessage = (m) => parentPort.postMessage(m);',
    'globalThis.addEventListener = (t, f) => parentPort.on("message", (d) => f({ data: d }));',
  ].join('\n');
  const worker = new Worker(`${shim}\n${source}`, { eval: true });
  return {
    post: (m) => worker.postMessage(m),
    onMessage: (f) => worker.on('message', f),
    onError: (f) => worker.on('error', (e) => f(String(e && e.message ? e.message : e))),
    terminate: () => void worker.terminate(),
  };
}

export const MANIFEST = {
  interfaces: {
    'demo.services.navigator': { methods: ['push', 'route'] },
    'demo.services.counter': { methods: ['next'] },
  },
};

// `scanSubset: false` skips the static scan so a test can put a forbidden spelling in front of the
// *runtime* lockdown, which is the half that has to hold when the lexer cannot see the spelling.
export function makeHost({ scanSubset = true } = {}) {
  return createMockHost({
    createWorker: nodeWorker,
    compile: (text, path) => compileMock(text, path),
    seed: 7,
    ...(scanSubset ? {} : { checkSubset: () => [] }),
  });
}

export const NAVIGATOR = `
interface Route { path: string; depth: number }
export default defineMock({
  interface: "demo.services.navigator",
  methods: {
    push: (args: unknown[], ctx: { callIndex: number }) => ({ value: { pushed: args[0], n: ctx.callIndex } }),
    route: (_args: unknown[], ctx: { random: () => number, nowMs: number }) =>
      ({ value: { path: "/inbox", r: ctx.random(), t: ctx.nowMs }, delayMs: 120 }),
  },
});
`;
export const COUNTER = `
export default defineMock({
  interface: "demo.services.counter",
  methods: {
    next: (_a, ctx) => { ctx.session.n = (ctx.session.n ?? 0) + 1; return { value: ctx.session.n }; },
  },
});
`;

