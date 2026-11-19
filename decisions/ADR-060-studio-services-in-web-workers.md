# ADR-060: Studio services run as Wasm modules in Web Workers behind one UI-agnostic protocol

- Status: Accepted (2026-11-19, owner). The owner gave the direction on 2026-11-19: the services are
  workers so the studio uses several threads; Chromium only for now; at V1 the studio is built with
  Tessera itself. The three questions were answered the same day (see "Owner answers").
- Date: 2026-11-19
- Related: [ADR-045](ADR-045-browser-hosted-studio.md) (browser-hosted studio),
  [ADR-054](ADR-054-studio-front-end-react-vite.md) (React + Vite chrome, the stepping stone),
  [ADR-055](ADR-055-studio-mock-services.md) (sandboxed mock worker),
  [ADR-061](ADR-061-compiler-owned-formatter.md) (the formatter service),
  [ADR-024](ADR-024-native-first-boundaries.md) (no JS in the engine),
  [`designer-studio-v1.md`](../docs/specs/designer-studio-v1.md) section 2.2 (platform constraints),
  [`designer-protocol-v1.md`](../docs/specs/designer-protocol-v1.md) section 2.1 (transport),
  [`mcp-server-v1.md`](../docs/specs/mcp-server-v1.md)

## Context

The studio is becoming an IDE: a language service (definitions, references, completions, hover,
rename), project-wide diagnostics, a formatter, the viewer and mock services. Today only the viewer
and the mock services are workers; the compile and diagnostics views run through Wasm modules the
page calls directly. A language service that parses on every keystroke on the main thread would
compete with typing and with the canvas.

Constraints already recorded, not new:

- GitHub Pages cannot set `Cross-Origin-Opener-Policy` / `Cross-Origin-Embedder-Policy`, so
  `SharedArrayBuffer` is unavailable and the Wasm build is single-threaded (`-sPTHREADS=0`;
  `designer-studio-v1.md` section 2.2). **"Several threads" therefore means several workers, each
  with its own module instance, talking by messages and transferable buffers**, not pthreads.
- The engine has no JS runtime in production (ADR-024). The analysis code is the C++ compiler front
  end (ADR-036), already compiled to Wasm for the diagnostics and scene views.
- The owner's V1 direction: the studio UI is rebuilt with Tessera. Whatever the services expose
  must not depend on React, the DOM or CodeMirror, or that rewrite throws the services away too.

## Decision

### 1. Every studio service is a Wasm module in its own Web Worker

| Service | Worker | Does |
|---|---|---|
| `viewer` | viewer worker (exists) | Renders the canvas; never reads or writes files |
| `analysis` | analysis worker | Parses with recovery, lowers, produces diagnostics, the symbol and span index, and answers language requests (definition, references, completions, hover, rename) |
| `format` | format worker | The canonical printer of ADR-061, answering with an edit list |
| `mock` | sandboxed mock worker (ADR-055) | Runs `*.mock.ts`; the only place JS the author wrote executes |

The page (and later the Tessera-built studio) is a client of these workers; it never calls a Wasm
export directly. `analysis` and `format` may share one worker at first (one module, two entry
points) and split when a measurement shows contention; the protocol does not change when they do.
**The analysis service is a pool from the start** (owner answer to Question 2), sized
`min(hardwareConcurrency - 2, 4)` and at least 1: a request for a file goes to the worker that holds
that file's cache (affinity by file id), whole-project checks fan out one file per worker, and the
page merges the answers by project version. The pool size is a setting the measurements may tune; a
pool of one is the degenerate case and must work.

### 2. One UI-agnostic message protocol

A new spec, `studio-services-v1.md`, extends the transport of `designer-protocol-v1.md` section 2.1:

- **Messages are plain data** (structured-cloneable): no DOM, React, CodeMirror or function values.
  A position is a UTF-16 offset plus a file id; a span is two positions; an edit is the
  `TextEdit` of the existing protocol.
- **Versioned and cancellable.** Every request carries the project version it was made against and
  a request id. A newer request of the same kind supersedes the older one (latest wins); a
  response carries the version it answers, and the client drops a response older than the text it
  shows. A worker never blocks the page.
- **The page owns the text.** A worker holds only a cache keyed by file version; the project
  snapshot or a file delta arrives as a transferable `ArrayBuffer`. A worker that is restarted
  rebuilds from the next snapshot, so a crash costs one request, not state.
- **Each service has a capability message** (name, protocol version, features), so the client can
  tell a missing feature from a failure, and the MCP server (ADR in `mcp-server-v1.md`) can reuse
  the same requests instead of a second API.

### 3. One analysis library, three hosts

The language service is a C++ library over the existing analyzer and lowering
(`src/compiler/analyzer`, `src/compiler/lowering`), not a second implementation. The same library is
compiled to Wasm for the analysis worker and natively for the CLI and the MCP server. A TypeScript
language service is not used (see Alternatives). An LSP stdio front end over the native library is
possible later and is not decided here.

### 4. Browser support, and where the studio goes at V1

Chromium (Chrome and Edge) is the supported browser for the IDE features now, because project
open/save uses the File System Access API. Other browsers keep the static viewer and the
read-only samples; their fallback is a later decision. At V1 the studio UI is rewritten with Tessera.
The workers and the protocol of section 2 carry over unchanged; only the page that talks to them is
replaced, and ADR-054's React chrome is explicitly a stepping stone.

## Consequences

- Typing never waits for a parse: analysis runs off the main thread and a stale answer is dropped.
- Parallelism is real but bounded: the analysis pool of section 1 parallelises whole-project work
  across files, the other services are one worker each, and there is no shared memory, so every
  worker holds its own copy of the files it analyses (memory grows with the pool size).
- Each Wasm module is loaded separately and counts against the preview size budget
  (`scripts/check_preview_budget.py`); modules load lazily and an IDE feature costs download only
  when the studio opens a project.
- The same requests serve the page, the Tessera-built studio and the MCP server, so a language
  feature is implemented once.
- A message protocol is more work than direct calls; the cost is paid once and is what makes the V1
  rewrite cheap.

## Alternatives

- **A TypeScript language service in a worker (tsserver style).** Rejected: a second implementation of
  the TSX dialect that drifts from the compiler (`bind::`, typed subset, `tsr-` classes), no reuse in
  the CLI, and the Tessera-built studio would still need the C++ analysis.
- **Services on the main thread.** Rejected: parse cost competes with typing and canvas input.
- **Wasm pthreads with `SharedArrayBuffer`.** Rejected for now: needs cross-origin isolation, which
  the static host cannot send; a service-worker shim has reload semantics. Revisit if the studio
  moves to a header-capable host.
- **One analysis worker, pool by measurement.** The earlier proposal; the owner chose the pool from
  the start (Question 2).
- **One worker for everything.** Rejected: a slow whole-project check would delay the format or
  completion of the file being typed.

## Owner answers (2026-11-19)

1. **Language service = the C++ front end in Wasm** (section 3): accepted as proposed.
2. **Pool of analysis workers: built from the start** (not the proposal, which was one worker and a
   pool by measurement). Section 1 describes it; the cost is memory per worker and the affinity and
   merge logic, which the first slice must include.
3. **Non-Chromium browsers:** the static viewer and read-only samples only, as proposed (section 4).

## Follow-up

- Write `studio-services-v1.md` (messages, versions, cancellation, capabilities) and link it from
  `designer-protocol-v1.md` section 2.1 and `designer-studio-v1.md`.
- Issues (created 2026-11-19): #567 (spec and harness), #568 (analysis library, pool and worker),
  #569 (diagnostics view behind the protocol), #570-#574 (index, definition, hover, completions,
  rename), #575 (Problems), #580-#583. The first slice (#568) must include the pool of section 1.
- Record Chromium-only in `designer-studio-v1.md` section 2.2.
