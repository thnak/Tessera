# Studio mock services v1

Status: proposed (design-time, V1-M4/D). Decisions: [ADR-054](../../decisions/ADR-054-studio-front-end-react-vite.md)
(studio front end), [ADR-055](../../decisions/ADR-055-studio-mock-services.md) (mock model and the
ADR-045 decision 7 exception). Related: [designer-studio-v1.md](designer-studio-v1.md) (project IO),
[designer-protocol-v1.md](designer-protocol-v1.md) (host/viewer transport),
[ADR-050 section 3.1 item 6](../../decisions/ADR-050/service-provisioning.md) (preview fakes),
[error-model-v1.md](error-model-v1.md) (code families). **Implementation is gated on the service seam**
(ADR-055 decision 9); this document defines the contract so the studio-side work and its tests can be
built against something stable.

Terms: **mock file** = a `*.mock.ts` project file; **mock module** = its transpiled JavaScript in the
sandbox; **interface id** = the reverse-DNS id a service interface declares in the project's service
manifest; **mock API** = the objects the sandbox exposes to a mock module (`defineMock`, `ctx`);
**call** = one service method invocation the engine-side fake records; **trace** = the ordered
sequence of calls and responses of one session.

## 1. Goals and non-goals

Goals: express app-service behavior in TypeScript inside the project folder; run it only in the
studio, deterministically and without network; let a developer see, edit and replay the responses the
preview consumes; keep the engine free of JavaScript (ADR-055).

Non-goals in v1: executing mocks in the Wasm engine, in a production binary or in the native CLI;
HTTP-level interception below the service seam; mock-to-mock imports or npm packages; asynchronous
handlers (the service call path is not async yet); type-checking with the project's service typings
(runtime validation only); a headless mock runner in `tessera-mcp` (a follow-up item); persisted
scenarios as project files (the console's responses are session state in v1).

## 2. File kind and project data

1. A mock is a UTF-8 file named `*.mock.ts`, project-relative, conventionally under `mocks/`. The
   extension is the rule: no other `.ts` file is accepted.
2. It is a **new accepted-file kind `mock`** in the shared policy that #391 (studio) and #406 (MCP)
   both link. Limits: at most 64 mock files, 1 MiB each, 8 MiB in total. An over-limit, a path with
   `..` or an absolute path, a symlink out of the root, or an encoding failure is a named error
   (section 10) and the file is not compiled.
3. **Mock text never reaches the viewer.** `project.load` and `source.update` keep their payloads;
   the host may report a mock's path, version and bound interface id in its own console, but the
   engine-side fake only ever receives responses (section 6).
4. The studio discovers mocks by scanning accepted files. Compilation is **lazy**: the transpiler and
   the sandbox start when the project has at least one mock and the mock console is opened or a call
   needs a response. A project without mocks pays nothing.
5. `tessera.json` gains **no field** in v1; binding is declared by the module (section 3). A project
   without a service manifest keeps its mocks **unbound** (section 5) and the preview is unaffected.
6. The MCP file tree lists a mock with `kind: "mock"`, `readable: true`, its size and digest; the
   contract does not expose its text to the viewer either.

## 3. Module contract

A mock module is a single ES module with **no imports in v1**. The transpiler wraps it with a prelude
that provides the mock API; the module must default-export one `defineMock(...)` value:

```ts
export default defineMock({
  interface: "demo.services.navigator",
  methods: {
    push: (args, ctx) => ({ value: null }),
    route: (args, ctx) => ({ value: { path: "/inbox", depth: 1 } }),
  },
});
```

1. `interface` is required: a non-empty reverse-DNS interface id. It must exist in the project's
   service manifest to bind (section 5); an unknown id is a named diagnostic, never a guess.
2. `methods` is a required object mapping a method name to a handler. A handler is
   `(args, ctx) => MockResult`, **synchronous**. `args` is an array of JSON values (the service
   method's positional arguments, in the manifest's order). `MockResult` is one of:
   - `{ value: <JSON value> }` — the call succeeded; the value is JSON-serializable and at most 64 KiB
     as canonical JSON;
   - `{ error: { code, message } }` — the call failed; `code` is a token `^[A-Z0-9_]{1,64}$` and
     `message` at most 1 KiB;
   - either of the above plus `delayMs` (0 to 60000, rounding below 1 ms rejected): the simulated
     latency, recorded in the trace. A synchronous preview call cannot wait, so `delayMs` is metadata
     for replay and for the future async path; it must not be presented as a live delay in v1.
3. `ctx` fields: `interfaceId`, `method`, `callIndex` (0-based per method of the session),
   `nowMs` (the studio's simulation time, an integer), `random()` (a seeded PRNG in `[0, 1)`),
   `session` (a JSON-serializable object persisted for the session, reset by a project reload or an
   explicit console reset). State must live in `ctx.session`; module-level mutable state is reset with
   the session and must not be relied on across reloads.
4. **Supported TypeScript subset:** type annotations, interfaces, type aliases, `const`/`let`, arrows
   and function expressions, objects/arrays, control flow, template strings, and JSON literals.
   **Rejected with a named error:** `import`/`export ... from`, dynamic `import()`, `async`/`await`,
   generators, classes, decorators, enums, namespaces, `eval`, `new Function`, top-level `await`,
   `require`, and any DOM/network/timer identifier from section 9. The transpiler is transpile-only:
   it does not type-check, and a type error is not a launch failure.
5. At most one mock may bind a given interface id per project; a duplicate is a named error and both
   files are unbound until one is removed.
6. Every violation is reported with the file, `line:column` and the code from section 10. A bad mock
   never blocks the preview: the fake keeps its default and the console shows the error.

## 4. Compile and sandbox

1. The studio host compiles a mock with a **lazy-loaded, pinned TypeScript transpiler** (candidate
   esbuild-wasm; a smaller transpiler is allowed if it passes this spec's checks). It is loaded only
   when a project contains mocks, so the cold download is unchanged (ADR-055 decision 5), and its
   size gets its own [preview_budget.json](../../tests/golden/perf/preview_budget.json) row.
2. Compilation runs in the sandbox worker, not on the UI thread; a compile is capped at 200 ms per
   file and 2 s per project. A transpiler error is `TESSERA_MOCK_TRANSPILE` with the mapped
   `line:column`.
3. The compiled module is produced as a **blob module that already contains the prelude** (mock API,
   shadowed globals), and a dedicated worker is started from it. No `eval`, no `new Function`, no
   `data:` script: the CSP must allow the blob worker (section 9). The worker is recreated when a
   mock file changes; its version is the file's version.
4. **Sandbox properties (normative):**
   - no DOM, no page globals, no `window`/`document`/`self` access beyond the mock API;
   - no network: `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`, `navigator.sendBeacon` and
     `importScripts` are shadowed with throwing stubs, and CSP restricts connections;
   - no wall clock: `Date`, `performance.now`, `setTimeout`, `setInterval`, `setImmediate` and
     `queueMicrotask` are shadowed; time comes from `ctx.nowMs`, ordering from the call sequence;
   - `Math.random` is replaced by the seeded PRNG (`ctx.random`);
   - the only data given to the worker is the mock source and its own `ctx.session`; it cannot read
     another project file;
   - the host validates every response (shape, JSON, size) before it is used or traced.
5. **Caps:** 1000 calls per session, 64 KiB per response, 10 s of mock CPU per session (safety timer
   on the worker, not a simulation clock), 50 ms per call. A cap or a hang terminates the worker,
   disables that mock with a named error, keeps the preview running, and records the event in the
   trace. A terminated worker is only restarted by an explicit console action or a project reload.
6. The sandbox is best-effort isolation on the web platform: the security review (section 11, ADR-055
   decision 4) must confirm the network and file boundaries; if it cannot, the owner falls back to
   ADR-054 option A (declarative mocks) for V1.

## 5. Interface binding

1. The mock's `interface` and its method names are checked against the project's service manifest
   (`ui/services.json`, whose spec does not exist yet; ADR-050 section 3.1 item 1 names its content).
   Until that spec lands, every mock is **unbound**: the console says so, the preview keeps the
   engine default, and nothing is claimed to work.
2. Binding status per mock is `bound`, `unbound` (no manifest or no such interface), or `error`
   (unknown method, duplicate interface, invalid module). Only `bound` mocks serve calls.
3. A method present in the manifest but absent from a mock is **unmocked**: a call to it returns the
   engine fake's default and is recorded as `unmocked` in the trace. A method in the mock but not in
   the manifest is `TESSERA_MOCK_METHOD_UNKNOWN` and that method is not served.
4. The engine-side fake is the ADR-050 section 3.1 item 6 fake: it owns the defaults and the call
   recording; the host owns the responses. The manifest ordering of arguments defines `args`.

## 6. Calls, responses and the bridge

1. **There is no synchronous round-trip.** The Wasm viewer cannot block on a worker (no
   `SharedArrayBuffer` on GitHub Pages, and the evaluator has no async service path), so the fake
   serves calls from a **response table** the host supplies and records calls the table does not
   cover.
2. **Capability `mock`.** The viewer advertises `"mock"` in `ready.capabilities` (protocol section 4)
   when it has the fake; the host sends `mock.enable` after `project.load` to say it will supply
   responses. Messages are additive to protocol version 1 and defined here (section 6.3); a peer
   without the capability ignores them and no mock runs.
3. **Protocol additions (normative):**

| Message | Direction | Payload | Behavior |
|---|---|---|---|
| `mock.enable` | host → viewer | `enabled` (boolean), optional `limit` (max calls, 1..1000) | Enables/disables call recording and the response table. Disabling clears the table and stops `mock.call` events |
| `mock.call` | viewer → host | `seq` (uint64 decimal string), `interface`, `method`, `args` (array), `unmocked` (boolean) | One recorded call, in call order, sent after the frame that produced it. The host answers with `mock.responses` or leaves it unmocked |
| `mock.responses` | host → viewer | `entries`: array of `{ interface, method, argsKey, occurrence, response }` where `response` is `{ value }` or `{ error: { code, message } }`, and `occurrence` is the 0-based repetition of the same `argsKey` | Replaces the table entries atomically before the next frame. `argsKey` is the canonical JSON of `args`; the host computes it and the viewer verifies it |
| `mock.reset` | host → viewer | `session` (boolean) | Clears the response table and the call counters; the next frame starts a new trace |

   Limits: `mock.call` args at most 8 KiB canonical JSON; at most 256 entries per `mock.responses`;
   each response at most 64 KiB; a session at most 1000 calls; a violation is `TESSERA_MOCK_LIMIT`
   and the table is left unchanged.
4. **Trace replay is the deterministic mode.** Given a trace (section 7), the host fills the table
   before `mock.enable` so the first frame already sees the recorded responses; a call that does not
   match the next trace entry is `TESSERA_MOCK_REPLAY_MISMATCH` and stops the replay (the preview
   continues with defaults).
5. **Record-then-edit is the interactive mode.** A call with no table entry is served by the fake's
   default; the console shows the call; the developer edits the response; the host sends the updated
   entry; the next frame uses it. The console states that the first frame of a new call uses the
   default.
6. **Determinism.** The response table is keyed by `(interface, method, argsKey, occurrence)`;
   identical calls in the same session and trace receive identical responses. The host applies a
   whole batch or nothing. No response depends on wall time.

## 7. Trace

A trace is versioned JSON, schema `1`:

```json
{
  "schema": 1,
  "mockFiles": [{ "path": "mocks/navigator.mock.ts", "version": 4, "sha256": "..." }],
  "entries": [
    { "seq": 0, "interface": "demo.services.navigator", "method": "push",
      "args": ["/compose"], "response": { "value": null }, "simTimeMs": 120 }
  ]
}
```

- `seq` is a 0-based counter in call order; `simTimeMs` is the studio's simulation time at the call.
- `sha256` is the hex digest of the mock file text, so a trace names the exact mocks it was recorded
  with; a replay against different digests warns but is allowed (the mismatch is shown).
- Limits: 10 000 entries and 4 MiB of canonical JSON; on overflow the trace stops recording with
  `TESSERA_MOCK_TRACE_LIMIT` and the session continues without tracing.
- The trace is session state; the console can download it. Tests may commit a trace under
  `tests/fixtures/mock/` as a fixture; nothing is uploaded and no trace is written to the project
  folder in v1.
- Replaying a trace is the acceptance mode for the mock feature: same trace, same frames, same
  structural JSON.

## 8. Studio console

The console is part of the React shell (ADR-054) and renders below the preview; it does not run mock
code:

1. a list of mocks with binding status (`bound`/`unbound`/`error`) and the reason;
2. a call table: `seq`, interface, method, args (collapsed), response state (`default`, `edited`,
   `unmocked`, `error`), and simulated time;
3. a response editor for the selected call (JSON), with validation and the section 10 error on a bad
   edit; applying is explicit (`Apply`, never on keystroke);
4. actions: reset session, disable/enable mocks, clear trace, download trace;
5. an empty state that names the missing service manifest when nothing can bind.

The console is keyboard operable, uses the repository's focus/role rules, and never shows a raw
exception: every failure is one of the codes in section 10 with its file/position where available.

## 9. Security requirements

These are the ADR-055 decision 4 gates and part of the acceptance of the implementation:

1. **CSP:** the studio page must allow the blob worker and restrict connections:
   `worker-src 'self' blob:`, `script-src 'self'`, `connect-src 'self'`, no `unsafe-eval`, no
   `unsafe-inline`. The exact meta-CSP is verified in the Pages artifact.
2. **Boundary:** the worker gets only the mock source, its own session state and the call arguments;
   it cannot read the project, the page, another worker or persistent storage.
3. **Network:** shadowed network functions plus CSP; the mock API exposes none. The security review
   must show that a hostile mock cannot send project data anywhere.
4. **Resource:** all caps of section 4.5 are enforced and a breach terminates the worker; the host
   never trusts shape, size or JSON validity of a response.
5. **Supply chain:** the transpiler is pinned and listed in
   [dependencies.md](dependencies.md) and [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).
6. **Fallback:** if the review cannot prove 2 and 3, V1 ships ADR-054 option A (declarative mocks)
   and this spec's sandbox sections are withdrawn.

## 10. Errors

Stable codes, added to the `TESSERA_MOCK_` family in [error-model-v1.md](error-model-v1.md) section 3:

| Code | When |
|---|---|
| `TESSERA_MOCK_FILE_LIMIT` | More than 64 mock files |
| `TESSERA_MOCK_FILE_TOO_LARGE` / `TESSERA_MOCK_TOTAL_TOO_LARGE` | 1 MiB / 8 MiB limits |
| `TESSERA_MOCK_PATH` | Absolute path, `..`, symlink escape, duplicate path |
| `TESSERA_MOCK_ENCODING` | Not valid UTF-8 |
| `TESSERA_MOCK_TRANSPILE` / `TESSERA_MOCK_SYNTAX` | Transpiler or subset rejection, with `line:column` |
| `TESSERA_MOCK_IMPORT_UNSUPPORTED` / `TESSERA_MOCK_ASYNC_UNSUPPORTED` / `TESSERA_MOCK_DYNAMIC_CODE` | Rejected constructs of section 3.4 |
| `TESSERA_MOCK_EXPORT_MISSING` | No default `defineMock(...)` export |
| `TESSERA_MOCK_INTERFACE_UNKNOWN` / `TESSERA_MOCK_METHOD_UNKNOWN` / `TESSERA_MOCK_DUPLICATE_INTERFACE` | Binding failures (section 5) |
| `TESSERA_MOCK_TIMEOUT` / `TESSERA_MOCK_CALL_LIMIT` / `TESSERA_MOCK_RESPONSE_TOO_LARGE` | Caps (section 4.5) |
| `TESSERA_MOCK_LIMIT` | A protocol message over its limit (section 6.3) |
| `TESSERA_MOCK_TRACE_LIMIT` / `TESSERA_MOCK_REPLAY_MISMATCH` | Trace limits and replay mismatch |
| `TESSERA_MOCK_SANDBOX_VIOLATION` | A shadowed global or forbidden operation was reached |

Each is `Error` severity except binding status (`Warn`): the preview continues with the engine
default. No mock error is `Fatal`.

## 11. Determinism and tests

- Every test uses the simulation clock and a fixed seed; no wall clock, no network, no real timers.
- Unit: file-kind policy and limits, path rejection, subset rejection per code, response validation,
  table keying and occurrence, cap enforcement, trace schema round trip.
- Parity/replay: a recorded trace replays to identical scene JSON and frame digests on the null RHI;
  a mismatched call fails with `TESSERA_MOCK_REPLAY_MISMATCH`; an unbound project leaves the frame
  identical to the no-mock build (the default path is byte-identical).
- Sandbox: a hostile fixture reaching `fetch`, `Date`, `Math.random`, `self`, `import()` and a
  runaway loop each produce the named code (or the timeout), never a silent pass.
- Browser check: the console lists, edits, applies and resets a response; recorded with Playwright
  when that lane exists (#375 owns it), otherwise by hand in the PR.
- CI: the Node-level tests need no browser; the sandbox tests run in the browser lane when present,
  and their absence is stated, never treated as a pass.

## 12. Limits (all normative)

| Limit | Value |
|---|---|
| Mock files per project | 64 |
| Bytes per mock file | 1 MiB |
| Bytes of mocks per project | 8 MiB |
| Props/handler positions | not applicable (methods only) |
| Call args | 8 KiB canonical JSON |
| Response | 64 KiB canonical JSON |
| Calls per session | 1000 |
| Entries per `mock.responses` | 256 |
| Mock CPU per session | 10 s (safety timer) |
| CPU per call | 50 ms |
| Compile per file / per project | 200 ms / 2 s |
| Trace entries / bytes | 10 000 / 4 MiB |

## 13. Versioning

- The module API is version 1 (`defineMock` and `ctx` as in section 3); optional fields may be added,
  a required-field or shape change needs a new version and a new spec revision.
- The trace schema is version 1; a reader rejects an unknown version rather than guessing.
- The protocol capability is `mock` version 1; message names/payloads follow section 6.3 and are
  additive to designer-protocol-v1.
- The `TESSERA_MOCK_*` codes are stable; new codes are added in the owning module and documented.

## 14. Open questions

1. **Transpiler.** esbuild-wasm versus a smaller transpiler; its measured compressed size, load time
   and the exact TypeScript subset it accepts.
2. **Sandbox mechanism.** Dedicated blob worker (this spec's assumption) versus a sandboxed iframe;
   whichever the security review proves stronger replaces section 4 without changing the module
   contract.
3. **Binding metadata.** A `mocks/` convention versus an explicit mapping in `tessera.json`; v1 uses
   the module's own `interface` field, which a future manifest spec may supplement.
4. **Persisted scenarios.** Whether edited responses become a project file (for review and tests) or
   stay session-only; v1 keeps them session-only and uses downloaded traces.
5. **Headless execution.** Running the same mocks under `tessera-mcp` in Node, and whether the trace
   format is the shared artifact there.
