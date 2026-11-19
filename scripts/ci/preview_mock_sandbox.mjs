// SPDX-License-Identifier: MIT
//
// The mock sandbox gate (issue #434, docs/specs/studio-mock-services-v1.md sections 4 and 11):
//
//   node scripts/ci/preview_mock_sandbox.mjs
//
// A hostile fixture reaching `fetch`, `Date`, `self`, `eval` and the Function constructor through
// spellings the static scan cannot see gets `TESSERA_MOCK_SANDBOX_VIOLATION` from the *runtime*
// lockdown and is disabled; a runaway loop, a call flood, a thrown handler, a non-JSON result and an
// oversized response are each a named code. The worker is the real `worker_threads` one running the
// exact text the page puts in a blob worker. A browser's network boundary rests on the page CSP and the
// security review (spec section 9); the browser lane (#375) owns that, and its absence is not a pass.

import { COUNTER, MANIFEST, NAVIGATOR, assert, finish, makeHost, test } from './mock_test_support.mjs';

// A hostile mock reaches for each forbidden thing at *call time* through spellings the static scan cannot
// see (computed keys, the Function constructor), so the runtime lockdown is what is under test.
const HOSTILE = (body) => `
export default defineMock({
  interface: "demo.services.navigator",
  methods: { push: (args, ctx) => { ${body} }, route: () => ({ value: 1 }) },
});
`;
const ATTACKS = {
  fetchByKey: 'const k = ["fe", "tch"].join(""); const f = globalThis[k]; return { value: f("https://example.test") };',
  clockByKey: 'const k = ["Da", "te"].join(""); const D = globalThis[k]; return { value: D.now() };',
  selfByKey: 'const k = ["se", "lf"].join(""); return { value: typeof globalThis[k].fetch };',
  functionCtor: 'const F = (() => 0)["constr" + "uctor"]; return { value: F("return 1")() };',
  evalByKey: 'const k = ["ev", "al"].join(""); return { value: globalThis[k]("1+1") };',
};

for (const [name, body] of Object.entries(ATTACKS)) {
  await test(`hostile fixture (${name}) gets the sandbox violation and is disabled, not a silent pass`, async () => {
    const host = makeHost({ scanSubset: false });
    await host.load([{ path: 'mocks/h.mock.ts', text: HOSTILE(body) }], MANIFEST);
    try {
      assert.equal(host.statuses()[0].status, 'bound', JSON.stringify(host.statuses()[0]));
      const entry = await host.call('demo.services.navigator', 'push', [], 0);
      assert.equal(entry.state, 'error');
      assert.equal(entry.response.error.code, 'TESSERA_MOCK_SANDBOX_VIOLATION', JSON.stringify(entry));
      assert.equal(host.statuses()[0].status, 'error');
      assert.equal(host.statuses()[0].disabled, true);
      const after = await host.call('demo.services.navigator', 'push', [], 1);
      assert.equal(after.state, 'unmocked', 'a disabled mock serves nothing');
    } finally {
      host.dispose();
    }
  });
}

await test('Math.random is the seeded generator and ctx.random continues the same sequence', async () => {
  const text = `export default defineMock({ interface: "demo.services.navigator", methods: {
    push: (_a, ctx) => ({ value: [Math.random(), ctx.random()] }), route: () => ({ value: 0 }) } });`;
  const read = async () => {
    const host = makeHost();
    await host.load([{ path: 'mocks/r.mock.ts', text }], MANIFEST);
    const entry = await host.call('demo.services.navigator', 'push', [], 0);
    host.dispose();
    return entry.response.value;
  };
  const first = await read();
  assert.deepEqual(first, await read());
  assert.notEqual(first[0], first[1]);
});

await test('a runaway handler is cut at 50 ms: TESSERA_MOCK_TIMEOUT, mock disabled, the other mock still serves', async () => {
  const host = makeHost();
  const spin = HOSTILE('for (;;) {}');
  await host.load(
    [
      { path: 'mocks/a.mock.ts', text: spin },
      { path: 'mocks/b.mock.ts', text: COUNTER },
    ],
    MANIFEST,
  );
  try {
    const entry = await host.call('demo.services.navigator', 'push', [], 0);
    assert.equal(entry.response.error.code, 'TESSERA_MOCK_TIMEOUT');
    assert.equal(host.statuses()[0].disabled, true);
    const ok = await host.call('demo.services.counter', 'next', [], 1);
    assert.deepEqual(ok.response, { value: 1 });
  } finally {
    host.dispose();
  }
});

await test('a thrown handler, a non-JSON result and an oversized response are named errors', async () => {
  const host = makeHost();
  const body = `
    if (args[0] === 0) { throw new Error("boom"); }
    if (args[0] === 1) { return { value: () => 1 }; }
    if (args[0] === 2) { return { value: "x".repeat(70000) }; }
    return { value: args[0] };`;
  await host.load([{ path: 'mocks/e.mock.ts', text: HOSTILE(body) }], MANIFEST);
  try {
    assert.equal((await host.call('demo.services.navigator', 'push', [0], 0)).response.error.code, 'TESSERA_MOCK_HANDLER_ERROR');
    assert.equal((await host.call('demo.services.navigator', 'push', [1], 0)).response.error.code, 'TESSERA_MOCK_SYNTAX');
    assert.equal((await host.call('demo.services.navigator', 'push', [2], 0)).response.error.code, 'TESSERA_MOCK_RESPONSE_TOO_LARGE');
    const ok = await host.call('demo.services.navigator', 'push', [3], 0);
    assert.deepEqual(ok.response, { value: 3 }, 'the mock survives a handler error');
  } finally {
    host.dispose();
  }
});

await test('ctx.session persists across calls and reset clears it, the table and the trace', async () => {
  const host = makeHost();
  await host.load([{ path: 'mocks/c.mock.ts', text: COUNTER }], MANIFEST);
  try {
    await host.call('demo.services.counter', 'next', [], 0);
    const two = await host.call('demo.services.counter', 'next', [], 1);
    assert.deepEqual(two.response, { value: 2 });
    host.reset();
    assert.equal(host.trace().entries.length, 0);
    const again = await host.call('demo.services.counter', 'next', [], 2);
    assert.deepEqual(again.response, { value: 1 });
    assert.equal(again.seq, 0);
  } finally {
    host.dispose();
  }
});

await test('the 1000-call cap disables the mock with TESSERA_MOCK_CALL_LIMIT', async () => {
  const host = makeHost();
  await host.load([{ path: 'mocks/c.mock.ts', text: COUNTER }], MANIFEST);
  try {
    let last;
    for (let i = 0; i < 1001; i += 1) {
      last = await host.call('demo.services.counter', 'next', [], i);
    }
    assert.equal(last.response.error.code, 'TESSERA_MOCK_CALL_LIMIT');
    assert.equal(host.statuses()[0].disabled, true);
    await host.restart('mocks/c.mock.ts');
    assert.equal(host.statuses()[0].status, 'bound');
    assert.deepEqual((await host.call('demo.services.counter', 'next', [], 2000)).response, { value: 1 });
  } finally {
    host.dispose();
  }
});


finish('mock sandbox');
