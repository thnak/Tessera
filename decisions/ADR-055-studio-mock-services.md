# ADR-055: Design-time mock services are TypeScript project files run in a sandboxed studio worker

- Status: Accepted (2026-11-16, owner chose option B: "về các đề xuất tôi chọn B")
- Date: 2026-11-16
- Related: [ADR-054](ADR-054-studio-front-end-react-vite.md) (studio stack; its open questions 2 and 3
  are resolved here), [ADR-045](ADR-045-browser-hosted-studio.md) (browser-hosted studio; decision 7 is
  narrowed here, decisions 1-6 stand), [ADR-050 section 3.1 item 6](ADR-050/service-provisioning.md)
  (designer preview fakes), [ADR-037](ADR-037-self-contained-wasm-designer.md) (self-contained Wasm
  viewer), [ADR-024](ADR-024-native-first-boundaries.md) (no production JS/React runtime — unchanged),
  [ADR-021](ADR-021-async-runtime.md) (async runtime), [ADR-046](ADR-046-web-target-for-demos.md) (web
  target), `docs/design/services/` (the service set, post-V1), GitHub issues #391, #406, #392, #396,
  #177 (AF5), #375

## Context

The studio is the product's design-time workspace; the owner wants to **test UI against mock services**
with mocks written as **TypeScript files inside the project folder**, held and compiled by the studio
chrome. Option B was chosen over a declarative-only mock (option A) and over a logger-first approach
(option C): it is the only option that lets a developer express arbitrary service behavior (stateful
sequences, errors, latency) in the language the project is already written in, and hot-edit it without
a native build.

**What the repository has today, and why B cannot work yet.**

- The project model is a folder with [`tessera.json`](../docs/specs/project-config-v1.md) at its root
  (ADR-014), loaded in the studio by
  [designer-studio-v1.md](../docs/specs/designer-studio-v1.md) section 7 (#391); accepted files are
  `tessera.json`, `.tsx`/`.jsx`, `.css`, theme JSON and declared fonts/images. **There is no
  TypeScript file kind**, and no single-file project format.
- The host owns the files and the viewer never reads them
  ([designer-protocol-v1.md](../docs/specs/designer-protocol-v1.md)); `project.load` carries `.tsx`,
  `.jsx`, `.css` and token files and has no message for state, input or services. A `state.patch` is
  explicitly not defined, and V1 has no state snapshot/restore.
- The service model is **post-V1** ([services/status-and-open-questions.md](../docs/design/services/status-and-open-questions.md)):
  `ui/services.json` has no spec and no reader in the tree
  ([service-injection.md](../docs/design/services/service-injection.md) section 11.6 item 3,
  [security-review-v1.md](../docs/specs/security-review-v1.md)), the TSX half of `useService<T>()` is
  not implemented, and the preview evaluator (`src/compiler/eval/`) has no service context at all.
- Running user code in the studio shell is forbidden today: ADR-045 decision 7 says "user code is IR
  only, never JavaScript", and designer-studio section 10 says "User code never becomes JavaScript".
  The ban in [ADR-024](ADR-024-native-first-boundaries.md) is about **production binaries**, so a
  design-time exception is possible, but it is an exception and needs its own decision (this ADR).
- GitHub Pages sends no cross-origin-isolation headers, so `SharedArrayBuffer` is unavailable and the
  Wasm build is single-threaded (ADR-045 decision 5,
  [designer-studio-v1.md](../docs/specs/designer-studio-v1.md) section 2.2). **A synchronous call from
  the engine to a worker is therefore impossible**, and the evaluator has no async service path.

So B is the right target model, but its engine-visible half is gated on the service seam that AF5 and
the services work have not built yet. This ADR fixes the model now so the studio-side work can be
designed, and records the gate honestly instead of promising a mock that cannot change a frame.

## Decision

1. **Mock files are project data.** A mock is `*.mock.ts` inside the project folder (conventionally
   under `mocks/`), a **new accepted-file kind**, project-relative, at most 1 MiB each and 64 per
   project. No other `.ts` file is accepted; the rule is the extension, not the directory. Mock text is
   never sent to the viewer; at most its path, version and interface id may appear in a host-side
   manifest.
2. **The studio host owns, compiles and runs them.** The page (React shell, ADR-054) holds the file,
   compiles it with a lazy-loaded in-browser TypeScript transpiler, and runs it in a **dedicated mock
   worker**: no DOM, no page globals, no access to other project files, no network. The worker gets an
   injected clock and timers driven by the studio's simulation/step control, seeded randomness, and a
   small mock API (registration, call recording, response). Caps on calls, payload size and CPU time
   are named errors, never silent truncation.
3. **The engine never executes TypeScript.** Service calls in the preview are served by the
   engine-side fake ADR-050 section 3.1 item 6 already names; the host supplies its behavior. The
   bridge is one of two modes, chosen by the mock spec: **(a) pre-resolved traces** — the host runs the
   mock against a declared call sequence and sends the resulting response data to the fake; or
   **(b) record-then-edit** — the fake returns a declared default, the host records the call, the
   developer or an agent edits the response, and later frames use it. **No design may assume a live
   synchronous round-trip** (impossible on Pages) or an async service call in the evaluator before that
   path exists.
4. **This is the one user-code exception in the studio.** It narrows ADR-045 decision 7 for
   `*.mock.ts` files only, and only in the studio; ADR-024 (no JS/React runtime in a production app)
   is untouched. Before implementation the security review must sign off on: CSP additions
   (`worker-src` for the compiled module, `connect-src` restricted to the site's own origin), proof
   that the worker inherits the document policy, no read access to project files or page globals, and
   bounds on CPU/memory/call volume. If the review cannot prove the network-exfiltration boundary, the
   owner falls back to ADR-054 option A (declarative mocks) for V1.
5. **The transpiler is lazy and measured.** It loads only when a project contains at least one mock, so
   the cold download and first paint are unchanged; it is pinned, listed in
   [dependencies.md](../docs/specs/dependencies.md) and
   [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), and its lazy module gets its own
   [preview_budget.json](../tests/golden/perf/preview_budget.json) row. esbuild-wasm is the candidate;
   a smaller transpiler is acceptable if it passes the spec's checks.
6. **Deterministic, replayable traces.** Mock execution uses the simulation clock, never the wall
   clock; no network; random is seeded. A trace (interface, method, arguments, response, timing) is
   recorded and replayable, so the same trace reproduces the same frames for studio runs, tests and
   MCP. Flakiness is a bug, not a tolerance.
7. **One file policy for the studio and MCP.** The accepted-kind/limit library shared by #391 (page)
   and #406 (MCP) owns the `*.mock.ts` rule; `project.load` stays unchanged (mock text is host-only),
   and any protocol addition is versioned in
   [designer-protocol-v1.md](../docs/specs/designer-protocol-v1.md) rather than invented privately.
8. **MCP sees the same files.** Agents may author and validate mocks as project data now; running them
   headlessly (Node) is a separate item that reuses the transpiler and the trace format. It is not
   claimed by this ADR.
9. **Sequencing is explicit.** The mock feature is gated, in order, on: (i) the `ui/services.json`
   manifest spec and generated `services.d.ts` typings; (ii) AF5's TSX `useService<T>()` lowering plus
   a preview `ServiceContext`; (iii) the service call path (pre-resolved or async) in the preview;
   (iv) this ADR's protocol additions and the mock spec; (v) the studio console. The studio-side
   infrastructure (file kind, transpiler, sandbox, trace, console) may be built and tested in
   isolation once (i) exists, but **no surface may claim a mock changes a previewed frame until
   (iii) lands**.

## Consequences

- **Positive.** Service behavior becomes expressible in the project's own language and hot-editable;
  UI states (loading, error, empty, stale data, latency) become design-time artifacts instead of
  manual C++ fakes; traces make preview behavior testable and reproducible; no native build and no
  network are involved; the engine stays free of JavaScript.
- **Negative.** A new runtime dependency (transpiler) and a lazy budget row; CSP changes and a real
  security review of executing project code in the page; protocol additions; and a delivery chain whose
  first half (services in the preview) is post-V1 work. Until then, B is architecture, not a feature.
- **Risks.** Executing untrusted project TS in the page is the largest new attack surface in the
  studio: exfiltration through network is the one to prove impossible or bound. The mitigation stack is
  worker isolation, a network-free mock API, inherited CSP and caps; the fallback is option A.
  A second risk is scope: mocks must not grow into a second app runtime or a general script host.
- **Compatibility.** No production impact: mocks are design-time only, never compiled into generated
  C++, never read by the engine, never sent to a viewer.

## Alternatives

- **A. Declarative mocks (data/scripts replayed by the engine fake).** Rejected by the owner: it cannot
  express stateful TypeScript logic. Kept as the documented fallback if the sandbox boundary cannot be
  proven for V1.
- **C. A component-scoped logger (`ILogger<Component>`-style) instead of mocks.** Rejected: observability
  cannot control service responses (ADR-054 open question 4); it remains a useful complementary
  feature and a separate slice.
- **A live synchronous bridge from the engine to a worker.** Rejected: no `SharedArrayBuffer` on
  GitHub Pages, the evaluator has no service call path, and blocking the UI thread is forbidden.
- **Compile each mock as part of the studio's Vite build.** Rejected: project files are runtime data,
  not build inputs; every edit would need a rebuild.
- **Execute mocks inside the Wasm engine or embed a JS engine.** Rejected: ADR-024's no-JS-runtime
  rule, size, and determinism; mocks are design-time.
- **A sandboxed iframe instead of a worker.** Not rejected; it is a candidate mechanism the mock spec
  may choose if the security review finds it stronger (open question 4).

## Open questions

1. **Mock API and binding.** The export shape, how a file declares the service interface ids it
   implements, and whether `tessera.json` names the mapping or a `mocks/` convention does; the
   interface ids come from the `ui/services.json` spec that does not exist yet.
2. **Bridge mode.** Pre-resolved traces or record-then-edit (decision 3), and the exact protocol
   messages, limits and version rules.
3. **Transpiler.** esbuild-wasm versus a smaller transpiler; measured size, load time and correctness
   for the TypeScript subset mocks may use (which subset is itself part of the spec).
4. **Sandbox mechanism and proof.** Dedicated worker versus sandboxed iframe; verification that the
   worker inherits the document CSP and that the mock API exposes no network; the security review's
   acceptance criteria.
5. **Trace format and reuse.** The recorded trace schema, whether it feeds `run_script`/MCP tests, and
   whether traces are committed as test fixtures.
6. **Headless execution.** Running the same mocks under `tessera-mcp` in Node, where the service seam is
   the native engine's, and what the mock API looks like there.

## Follow-up

1. **Specs:** `docs/specs/studio-mock-services-v1.md` (mock API, file kind, binding, caps, trace,
   determinism) and the missing `ui/services.json` / `services.d.ts` spec; protocol additions in
   [designer-protocol-v1.md](../docs/specs/designer-protocol-v1.md).
2. **Framework prerequisites:** AF5 TSX `useService<T>()` lowering, a preview `ServiceContext`, and the
   service call path (pre-resolved or async).
3. **Studio slice:** the `*.mock.ts` file kind in the shared #391/#406 policy, the lazy transpiler, the
   mock worker/sandbox, the console UI and the trace view; `pages.yml` and the budget row; the browser
   check.
4. **Security:** a review item for the CSP changes and the sandbox boundary, from the same review
   process that produced #397-#409; if it fails, fall back to option A for V1.
5. **Docs:** `dependencies.md`, `THIRD_PARTY_NOTICES.md`, `designer-studio-v1.md` (a mock-service
   section), `compiler-wasm.md` (page modules), and AGENTS.md Current focus.
6. **Re-baseline:** #391 owns the project store that holds mocks; #392's shell hosts the console; #396
   gains the MCP-side mock tools only after 1 and 2 land.
