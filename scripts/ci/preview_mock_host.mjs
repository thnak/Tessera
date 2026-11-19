// SPDX-License-Identifier: MIT
//
// The studio mock-service host (issue #434, ADR-055, docs/specs/studio-mock-services-v1.md):
//
//   node scripts/ci/preview_mock_host.mjs
//
// What this proves, with no browser and no network:
//
//   1. **File kind and limits** (spec section 2): `*.mock.ts` is the `mock` kind, a plain `.ts` stays
//      locked, and 64 files / 8 MiB are named refusals.
//   2. **The subset** (section 3.4): every rejected construct yields its stable code with `line:column`;
//      a forbidden word inside a string, comment or template text is not a violation.
//   3. **The response table and trace** (sections 6 and 7): canonical keys, occurrences, response
//      validation, trace schema round trip, replay mismatch.
//   4. **The host** (binding, serving, determinism, response edits). The hostile-mock and cap cases
//      (the page runs the same text in a blob worker). A hostile fixture reaching `fetch`, `Date`,
//      `Math.random`-as-wall-clock, `self`, `import()`, `new Function`, a constructor escape and a
//      runaway loop each produce a named code, never a silent pass; the mock is disabled and a second
//      mock keeps working.
//   5. **Determinism**: two sessions with the same seed and calls produce byte-identical traces.
//
// **What it does not prove.** The network boundary of a *browser* worker rests on the page's CSP and the
// security review (spec section 9); `worker_threads` is not a browser. The browser lane (#375) owns that
// run, and its absence is stated here rather than counted as a pass.

import { FILE_KIND, LIMITS, classifyPath, isTextKind, limitMessage } from '../../designer/web/project-paths.js';
import { buildProject } from '../../designer/web/project-io.js';
import { checkSubset } from '../../designer/web/mock/subset.js';
import { ResponseTable, argsKeyOf, canonicalJson, validateResponse } from '../../designer/web/mock/response.js';
import { ReplayCursor, fillTable, readTrace, toDownload } from '../../designer/web/mock/trace.js';
import { COUNTER, MANIFEST, NAVIGATOR, assert, finish, makeHost, test } from './mock_test_support.mjs';

// --- 1. file kind and limits ----------------------------------------------------------------------

await test('a *.mock.ts file is the mock kind, text and editable; a plain .ts stays locked', () => {
  assert.equal(classifyPath('mocks/navigator.mock.ts'), FILE_KIND.mock);
  assert.equal(classifyPath('anywhere/deep/x.mock.ts'), FILE_KIND.mock);
  // A plain .ts is a text file the studio does not accept: locked, opened view-only (#391).
  assert.equal(classifyPath('mocks/util.ts'), FILE_KIND.viewOnly);
  assert.equal(classifyPath('x.mock.tsx'), FILE_KIND.source);
  assert.ok(isTextKind(FILE_KIND.mock));
});

await test('more than 64 mock files, or 8 MiB of them, is a named refusal before any read', async () => {
  const raw = (count, size) =>
    Array.from({ length: count }, (_, i) => ({
      path: `mocks/m${i}.mock.ts`,
      size,
      read: async () => {
        throw new Error('must not be read');
      },
    }));
  await assert.rejects(buildProject(raw(LIMITS.maxMockFiles + 1, 10)), (e) => e.limit === 'maxMockFiles');
  await assert.rejects(
    buildProject(raw(10, LIMITS.maxFileBytes)),
    (e) => e.limit === 'maxMockBytes',
  );
  assert.match(limitMessage('maxMockFiles', 65), /TESSERA_MOCK_FILE_LIMIT/);
  assert.match(limitMessage('maxMockBytes', 1), /TESSERA_MOCK_TOTAL_TOO_LARGE/);
});

// --- 2. the subset --------------------------------------------------------------------------------

const REJECTED = [
  ['import x from "y";', 'TESSERA_MOCK_IMPORT_UNSUPPORTED', 1, 1],
  ['const m = await import("y");', 'TESSERA_MOCK_ASYNC_UNSUPPORTED', 1, 11],
  ['export const a = 1;', 'TESSERA_MOCK_IMPORT_UNSUPPORTED', 1, 1],
  ['const f = async () => 1;', 'TESSERA_MOCK_ASYNC_UNSUPPORTED', 1, 11],
  ['function* g() {}', 'TESSERA_MOCK_ASYNC_UNSUPPORTED', 1, 1],
  ['class A {}', 'TESSERA_MOCK_SYNTAX', 1, 1],
  ['enum E { A }', 'TESSERA_MOCK_SYNTAX', 1, 1],
  ['namespace N {}', 'TESSERA_MOCK_SYNTAX', 1, 1],
  ['@dec\nconst x = 1;', 'TESSERA_MOCK_SYNTAX', 1, 1],
  ['eval("1")', 'TESSERA_MOCK_DYNAMIC_CODE', 1, 1],
  ['const f = new Function("return 1");', 'TESSERA_MOCK_DYNAMIC_CODE', 1, 15],
  ['const r = require("fs");', 'TESSERA_MOCK_DYNAMIC_CODE', 1, 11],
  ['fetch("https://x.test")', 'TESSERA_MOCK_SANDBOX_VIOLATION', 1, 1],
  ['const t = Date.now();', 'TESSERA_MOCK_SANDBOX_VIOLATION', 1, 11],
  ['self.postMessage(1)', 'TESSERA_MOCK_SANDBOX_VIOLATION', 1, 1],
  ['const c = (() => 0).constructor;', 'TESSERA_MOCK_SANDBOX_VIOLATION', 1, 21],
  ['\n\n  setTimeout(() => 1, 0)', 'TESSERA_MOCK_SANDBOX_VIOLATION', 3, 3],
];

await test('each rejected construct has its stable code and line:column', () => {
  for (const [source, code, line, column] of REJECTED) {
    const found = checkSubset(source);
    assert.ok(found.length > 0, `no finding for ${JSON.stringify(source)}`);
    assert.deepEqual(
      [found[0].code, found[0].line, found[0].column],
      [code, line, column],
      JSON.stringify(source),
    );
  }
});

await test('forbidden words in strings, comments, templates, keys and members are not violations', () => {
  const ok = [
    'const s = "fetch Date import class async";',
    "const s = 'eval(1)';",
    '// fetch and Date and import\nconst x = 1;',
    '/* class A {} */ const x = 1;',
    'const t = `fetch ${1 + 1} Date`;',
    'const o = { class: 1, async: 2, import: 3 }; const v = o.class + o.async;',
    'const r = /fetch\\//.test("x"); const d = 4 / 2;',
    NAVIGATOR,
    COUNTER,
    'export default defineMock({ interface: "a.b", methods: {} });',
  ];
  for (const source of ok) {
    assert.deepEqual(checkSubset(source), [], source);
  }
});

// --- 3. response table and trace ------------------------------------------------------------------

await test('canonical JSON sorts keys; args keys and occurrences identify identical calls', () => {
  assert.equal(canonicalJson({ b: 1, a: [2, { d: 1, c: 2 }] }), '{"a":[2,{"c":2,"d":1}],"b":1}');
  assert.equal(argsKeyOf([{ y: 1, x: 2 }]), '[{"x":2,"y":1}]');
  assert.equal(argsKeyOf([undefined]), null);
  assert.equal(argsKeyOf(['x'.repeat(9000)]), null);
  const table = new ResponseTable();
  assert.equal(table.nextOccurrence('i', 'm', '[]'), 0);
  assert.equal(table.nextOccurrence('i', 'm', '[]'), 1);
  assert.equal(table.nextOccurrence('i', 'm', '[1]'), 0);
});

await test('a handler result is validated for shape, JSON, code token, delay and size', () => {
  assert.equal(validateResponse({ value: { a: 1 } }).ok, true);
  assert.equal(validateResponse({ error: { code: 'NOT_FOUND', message: 'x' }, delayMs: 5 }).delayMs, 5);
  for (const bad of [
    null, [], {}, { value: 1, error: {} }, { value: 1, extra: 1 }, { value: 1, delayMs: -1 },
    { value: 1, delayMs: 60001 }, { error: { code: 'lower', message: 'x' } },
    { error: { code: 'A', message: 'x'.repeat(1025) } }, { value: () => 1 }, { value: undefined },
  ]) {
    assert.equal(validateResponse(bad).ok, false, JSON.stringify(bad));
  }
  const big = validateResponse({ value: 'x'.repeat(65 * 1024) });
  assert.equal(big.code, 'TESSERA_MOCK_RESPONSE_TOO_LARGE');
});

await test('the trace round-trips, rejects an unknown schema, and a diverging call stops replay', () => {
  const trace = {
    schema: 1,
    mockFiles: [{ path: 'mocks/a.mock.ts', version: 1, sha256: 'ab' }],
    entries: [
      { seq: 0, interface: 'i', method: 'm', args: [1], response: { value: 2 }, simTimeMs: 10 },
      { seq: 1, interface: 'i', method: 'm', args: [1], response: { value: 3 }, simTimeMs: 20 },
    ],
  };
  assert.deepEqual(readTrace(JSON.parse(JSON.stringify(trace))), trace);
  assert.throws(() => readTrace({ ...trace, schema: 2 }), /schema/);
  assert.throws(() => readTrace({ ...trace, entries: [{ ...trace.entries[0], seq: 5 }] }), /malformed/);
  const table = new ResponseTable();
  fillTable(table, trace);
  assert.deepEqual(table.get('i', 'm', '[1]', 0), { value: 2 });
  assert.deepEqual(table.get('i', 'm', '[1]', 1), { value: 3 });
  const cursor = new ReplayCursor(trace);
  assert.equal(cursor.next('i', 'm', [1]), null);
  assert.match(cursor.next('i', 'm', [999]), /does not match/);
  assert.match(cursor.next('i', 'm', [1]), /already stopped/);
  assert.equal(toDownload({ ...trace, entries: [] }).entries.length, 0);
});

// --- 4. the host and the sandbox ------------------------------------------------------------------

await test('a bound mock serves calls with its response, delayMs as metadata, and a deterministic seed', async () => {
  const host = makeHost();
  await host.load([{ path: 'mocks/navigator.mock.ts', text: NAVIGATOR, version: 1 }], MANIFEST);
  try {
    const [status] = host.statuses();
    assert.equal(status.status, 'bound', JSON.stringify(status));
    assert.deepEqual(status.methods, ['push', 'route']);
    const a = await host.call('demo.services.navigator', 'push', ['/compose'], 120);
    assert.deepEqual(a.response, { value: { pushed: '/compose', n: 0 } });
    assert.equal(a.state, 'default');
    const b = await host.call('demo.services.navigator', 'route', [], 240);
    assert.equal(b.delayMs, 120);
    assert.equal(b.response.value.t, 240);
    assert.ok(b.response.value.r >= 0 && b.response.value.r < 1);
    const c = await host.call('demo.services.navigator', 'push', ['/x'], 300);
    assert.equal(c.response.value.n, 1, 'callIndex counts per method');
  } finally {
    host.dispose();
  }
});

await test('two sessions with the same seed and calls produce byte-identical traces', async () => {
  const run = async () => {
    const host = makeHost();
    await host.load([{ path: 'mocks/navigator.mock.ts', text: NAVIGATOR, version: 1 }], MANIFEST);
    for (let i = 0; i < 3; i += 1) {
      await host.call('demo.services.navigator', 'route', [i], i * 10);
    }
    const out = JSON.stringify(toDownload(host.trace()));
    host.dispose();
    return out;
  };
  assert.equal(await run(), await run());
});

await test('without a manifest every mock is unbound and no call is served (the preview default)', async () => {
  const host = makeHost();
  await host.load([{ path: 'mocks/navigator.mock.ts', text: NAVIGATOR, version: 1 }], null);
  try {
    assert.equal(host.statuses()[0].status, 'unbound');
    assert.match(host.statuses()[0].reason, /no service manifest/);
    const entry = await host.call('demo.services.navigator', 'push', [1], 0);
    assert.equal(entry.state, 'unmocked');
    assert.equal(entry.response, null);
  } finally {
    host.dispose();
  }
});

await test('binding: unknown interface is unbound, unknown method is an error, duplicates disable both', async () => {
  const host = makeHost();
  const orphan = COUNTER.replace('demo.services.counter', 'demo.services.other');
  const extra = NAVIGATOR.replace('route:', 'bogus:');
  await host.load(
    [
      { path: 'mocks/a.mock.ts', text: orphan },
      { path: 'mocks/b.mock.ts', text: extra },
    ],
    MANIFEST,
  );
  const [a, b] = host.statuses();
  assert.equal(a.status, 'unbound');
  assert.ok(a.diagnostics.some((d) => d.code === 'TESSERA_MOCK_INTERFACE_UNKNOWN'));
  assert.equal(b.status, 'error');
  assert.ok(b.diagnostics.some((d) => d.code === 'TESSERA_MOCK_METHOD_UNKNOWN'));
  await host.load(
    [
      { path: 'mocks/a.mock.ts', text: NAVIGATOR },
      { path: 'mocks/b.mock.ts', text: NAVIGATOR },
    ],
    MANIFEST,
  );
  for (const status of host.statuses()) {
    assert.equal(status.status, 'error');
    assert.ok(status.diagnostics.some((d) => d.code === 'TESSERA_MOCK_DUPLICATE_INTERFACE'));
  }
  host.dispose();
});

await test('a bad mock never blocks another: syntax, transpile and missing export are per-file errors', async () => {
  const host = makeHost();
  await host.load(
    [
      { path: 'mocks/a.mock.ts', text: 'import x from "y";' },
      { path: 'mocks/b.mock.ts', text: 'const = ;' },
      { path: 'mocks/c.mock.ts', text: 'const x = 1;' },
      { path: 'mocks/d.mock.ts', text: COUNTER },
    ],
    MANIFEST,
  );
  try {
    const [a, b, c, d] = host.statuses();
    assert.equal(a.diagnostics[0].code, 'TESSERA_MOCK_IMPORT_UNSUPPORTED');
    assert.equal(b.diagnostics[0].code, 'TESSERA_MOCK_TRANSPILE');
    assert.ok(b.diagnostics[0].line >= 1);
    assert.equal(c.diagnostics[0].code, 'TESSERA_MOCK_EXPORT_MISSING');
    assert.equal(d.status, 'bound');
    const entry = await host.call('demo.services.counter', 'next', [], 0);
    assert.deepEqual(entry.response, { value: 1 });
  } finally {
    host.dispose();
  }
});

await test('editing a response validates first, applies explicitly and marks the call edited', async () => {
  const host = makeHost();
  await host.load([{ path: 'mocks/n.mock.ts', text: NAVIGATOR }], MANIFEST);
  try {
    const entry = await host.call('demo.services.navigator', 'push', ['/a'], 0);
    assert.equal(host.editResponse(entry.seq, '{ nope').code, 'TESSERA_MOCK_SYNTAX');
    assert.equal(host.editResponse(entry.seq, '{"value":1,"error":{}}').code, 'TESSERA_MOCK_SYNTAX');
    assert.equal(entry.state, 'default', 'a rejected edit changes nothing');
    assert.equal(host.editResponse(entry.seq, '{"error":{"code":"OFFLINE","message":"no network"}}'), null);
    assert.equal(entry.state, 'edited');
    assert.deepEqual(host.table.get('demo.services.navigator', 'push', '["/a"]', 0), {
      error: { code: 'OFFLINE', message: 'no network' },
    });
  } finally {
    host.dispose();
  }
});

await test('the page-side files stay framework-independent: no DOM, no network in the mock modules', async () => {
  const { readFileSync, readdirSync } = await import('node:fs');
  const dir = new URL('../../designer/web/mock/', import.meta.url);
  for (const file of readdirSync(dir)) {
    const text = readFileSync(new URL(file, dir), 'utf8');
    // These two name the forbidden identifiers on purpose: one to reject them, one to lock them.
    if (file === 'sandbox-source.js' || file === 'subset.js') {
      continue;
    }
    assert.ok(!/\bdocument\.[a-zA-Z]/.test(text), `${file}: document`);
    assert.ok(!/\bfetch\(|XMLHttpRequest|WebSocket/.test(text), `${file}: network`);
  }
});

finish('mock host');
