# ADR-077: One `tessera` command for design time: verbs over the existing drivers, a native `watch` loop, no second engine

- Status: Proposed (2026-11-28). Written from the CLI survey requested by the owner ("we have Wasm for
  design time, what about the CLI"); the owner Accepts.
- Date: 2026-11-28
- Related: [ADR-045](ADR-045-browser-hosted-studio.md) (browser studio and the stdio MCP adapter),
  [ADR-061](ADR-061-compiler-owned-formatter.md) (`tessera fmt`),
  [ADR-060](ADR-060-studio-services-in-web-workers.md), [ADR-024](ADR-024-native-first-boundaries.md),
  [`devtools-protocol-v1.md`](../docs/specs/devtools-protocol-v1.md) (hot reload, section 4),
  [`headless-automation-v1.md`](../docs/specs/headless-automation-v1.md) (`tessera test`, `headless`),
  [`mcp-server-v1.md`](../docs/specs/mcp-server-v1.md), [`designer-studio-v1.md`](../docs/specs/designer-studio-v1.md),
  [`docs/guides/project-generator.md`](../docs/guides/project-generator.md)
- Terms: a **driver** is the in-process function a tool calls (`diagnose_source`, the IR writer, `scene_prepare`,
  `scene_frame`, the harness script runner). A **verb** is a subcommand of one `tessera` executable.

## Context

Design time has a browser half and a scattering of native tools. The browser half is the Wasm studio
(ADR-045, ADR-054): evaluate TSX in memory, show the scene on a WebGPU canvas, simulate device chrome.
The native half is real but fragmented (state at the time of writing, from the tree):

- `tessera_codegen`: C++ generation (`--write`, `--check`), IR (`--emit-ir`), diagnostics JSON, library components.
  It is also the target of the Wasm compiler lane (`compiler-wasm.md` section 2), so it must stay free of the engine.
- `tessera_scene_preview`: in-memory scene snapshot JSON and click scripts, 320x240 by default. Parity gates only.
- `tessera_scene_frame`: PNG of the same pipeline (Vulkan or null backend). Parity gates only.
- `tessera_harness_cli` ("the `tessera` CLI" in the harness spec): `test` and `headless`, a stdio protocol server.
- `tessera-mcp`: the agent adapter over the same drivers; `screenshot`, `apply_edits` and `run_script` are declared, not implemented.
- `tessera new` (project generator), `tessera_port_check`, `tessera_schema_dump`; `tessera fmt` is accepted (ADR-061) and unbuilt (#576).

What is missing is the loop a designer or an agent actually lives in: edit a file, see the result. No native
command watches sources, recompiles, and shows or re-renders; the hot-reload rules of the devtools protocol
have no native driver beyond the compiler error overlay; a human has to know five binary names. The risk of
fixing this by feature accretion is a third copy of the evaluation path (browser, MCP, CLI each wired by hand).

## Decision

1. **One executable, `tessera`, with verbs.** Verbs map one to one onto drivers that already exist; a verb adds
   argument parsing and output formatting, never a second implementation. Initial set:
   `new`, `check` (diagnostics JSON or human text, exit codes as `tessera_codegen`), `ir`, `gen` (`--check` / `--write`),
   `fmt` (ADR-061), `snapshot`, `frame` (PNG, backend choice), `test` and `headless` (the existing harness verbs),
   `mcp` (serve the MCP adapter on stdio), `watch`. Existing executables keep working as aliases for one release;
   `tessera_codegen` stays a separate binary because the Wasm lane builds it (the engine-free rule above), and the
   `tessera` verbs `check`, `ir` and `gen` call its library, not its `main`.
2. **Output is machine-first.** Every verb has `--json` printing the same documents the MCP tools return
   (byte-identical, as `mcp_diagnostics_parity` already requires for diagnostics) and human text otherwise; exit codes
   are `0` ok, `1` diagnostics or drift, `2` usage or setup, as today.
3. **`tessera watch` is the native design loop.** It watches the project's `.tsx`, `.css` and `tessera.json`, and on a
   change runs compile, scene evaluation and (optionally) a frame, then publishes the result: diagnostics to the terminal
   and, with `--serve`, a devtools-protocol session (JSON lines on stdio or the per-user socket with a token) so a
   viewer, an agent or the studio can follow. It applies the devtools hot-reload rules unchanged: a style-only change is a
   patch, anything else a structural reconcile with state preserved by component and key, and a failed compile keeps the
   last good frame and shows the error overlay. It creates no thread of its own beyond the runtime's, polls with
   `wait_readable`-style timed waits (no busy loop), and coalesces bursts of file events into one rebuild.
4. **Windowed native preview is a mode of the same loop, not a new app.** `tessera watch --window` opens the app window
   driver (the one `--frames` runs use) on the evaluated scene, so a developer without a browser sees real Vulkan output.
   Real GPU output is evidence, never a golden (CLAUDE.md, Goldens).
5. **Close the MCP gaps through the verbs.** `screenshot` is `frame`, `run_script` is `test --script`, and `apply_edits`
   is `source.patch` once the style override layer (#536) lands; the MCP tool table is updated when each becomes real.
6. **The studio stays the richer surface.** Device skins, fold postures, mock services and the dock layout remain
   browser features (ADR-045, ADR-055). The CLI exposes the viewport (`--size WxH@DPI`) and the environment of
   ADR-074 but not simulated chrome; a CLI frame is the content, not the device.
7. **Security and portability.** `watch --serve` follows the devtools security rules (local only, session token, one
   primary client, extra clients rejected). The CLI reads only inside the project root (the shared project-files policy
   with its `TESSERA_SEC_` refusals) and downloads nothing.

## Alternatives considered

- **Keep separate binaries, document them.** Cheapest, and what exists. Rejected: the loop is the missing piece, and five
  names plus three declared-only MCP tools is the discoverability problem the owner raised.
- **Make the CLI a thin client of the browser studio (Playwright).** Rejected: it needs a browser at design time and
  keeps the native path untested; ADR-024 says production has no JS runtime and the native engine is the source of truth.
- **A long-running daemon with a project index.** Deferred: `watch` with an in-process cache gives the benefit without a
  background process to secure and version.
- **Fold `tessera_codegen` into `tessera` and ship one binary.** Rejected for now: it would link the engine into the
  Wasm compiler artifact (`compiler-wasm.md` section 2).

## Consequences

- One documented entry point for humans and agents; the MCP tool table and the CLI cannot drift because both call the
  same drivers and share parity tests.
- A native watch loop makes the devtools hot-reload rules testable without a browser (a manual clock and a fake
  file watcher drive it deterministically).
- Cost: argument parsing and packaging work, a file-watching seam in `src/platform` (inotify,
  ReadDirectoryChangesW; polling fallback), and keeping the alias period honest.

## Evidence required before Accepted

Spike: `tessera watch` over `tessera_scene_preview`'s drivers with a polling watcher, measuring edit-to-diagnostics and
edit-to-frame latency on the counter template (no wall-clock assertions in tests; numbers recorded in this ADR), and
showing a failed compile keeps the last frame. Implementation issues are opened after Acceptance (verbs, watch,
windowed mode, MCP gaps), each with the farming-protocol checklist.

## Open questions

1. Install name and packaging: `tessera` on `PATH` from the SDK, or `cmake --install` only?
2. Should `watch --serve` reuse the studio's `designer-protocol` messages instead of devtools-protocol JSON lines?
3. Which verbs belong in the V1 milestone and which after it (`--window` is the large one).
