# Headless automation v1

Status: proposed (Big Steps B2 and C, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big
Steps"). Decision:
[ADR-027](../../decisions/ADR-027-agent-automation-and-audits.md) (numeric automation IDs,
headless harness, Lighthouse-style audits). Companion to
[`testing.md`](../design/testing.md), [`golden-tests-v1.md`](golden-tests-v1.md), and
[`automation-ids-v1.md`](automation-ids-v1.md).

A deterministic headless harness lets coding agents and CI drive the real UI without a window,
GPU, OS event loop, or network.

## 1. Modes

| Mode | Backend | Output | Use |
|---|---|---|---|
| `structural` | null | snapshot, layout/style/paint stats, events | fast agent tests |
| `golden` | software/lavapipe (Mesa software Vulkan driver) | snapshot + PNG + report | visual regression |
| `gpu-smoke` | Vulkan | frames/metrics, tolerance | backend smoke |
| `audit` | software or GPU | Lighthouse-style audit JSON (see [`lighthouse-audit-v1.md`](lighthouse-audit-v1.md)) | performance/quality gates |

Goldens always come from the software path
([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md), deterministic
headless golden tests). `gpu-smoke` never updates baselines.

## 2. Harness

- Fake HAL (hardware/OS abstraction layer, see [`hal.md`](../design/hal.md)):
  window/display/input/power/lifecycle/timer; no real OS callbacks.
- Fixed size, DPI/scale, fonts, locale, time zone, and asset set.
- Simulated monotonic clock and fixed timestep.
- Deterministic executor; no detached tasks/network/system fonts.
- Real UI/style/layout/paint/renderer code; only the platform/GPU edges are fake/software.
- App config loaded from `tessera.json`; automation manifest loaded from `generated/`.
  - **Discovery.** There is no `--app` flag. A script's `app.config` (section 4, a safe relative
    path from the working directory, normally `tessera.json`) selects the project. The manifest is
    `<dir of app.config>/<codegen.outDir, default generated>/automation.manifest.json`
    ([`automation-ids-v1.md`](automation-ids-v1.md) section 2.1). A script without `app.config`
    runs the built-in fixture (section 6.8, ids 9001-9010).
  - **Validation.** `tessera.json` is at most 256 KiB, must be a JSON object with
    `schemaVersion: 1` and an `app` object (the full schema is
    [`project-config-v1.md`](project-config-v1.md), enforced by the generator; the harness reads
    only these fields plus `codegen.outDir`). The manifest is at most 8 MiB and 20,000 nodes,
    `manifestVersion` must be `1`, ids are non-zero decimal uint64 strings (no leading zeros),
    `uiId`s and node `path`s are unique, strings are at most 512 bytes.
  - **Scene.** `ManifestScene` (`include/tessera/automation/manifest_scene.hpp`) builds one scene
    node per manifest node with the manifest `uiId` as its automation id and `kind` as its element
    kind; the tree shape comes from the structural paths (a node's parent is its path minus the
    last segment; component roots attach to the scene root). It is a structural description, not
    the compiled app: Buttons are focusable, TextInput/TextArea accept `type`, Scroll/VirtualList
    scroll, other kinds are inert, and the app's event handlers do not run. Default sizes come
    from a built-in stylesheet, followed by the project's own `ui.styles` CSS files
    ([`project-config-v1.md`](project-config-v1.md)). Snapshots report each node's manifest `path` and `componentId`
    (`annotations_from_manifest` feeds `EngineAutomationHost::Config`), not the live-tree path;
    a node with a static `aria-label`, `label` prop or literal text has an optional manifest `name`, which the scene node carries as
    its `aria-label` so snapshots report it as `name` (dynamic labels are omitted). A missing or invalid config or manifest is exit `2` with a
    deterministic `tessera <command>: <Code>: <message>` line (`ConfigUnreadable`, `ConfigTooLarge`,
    `ConfigInvalid`, `ManifestMissing`, `ManifestTooLarge`, `ManifestInvalid`,
    `ManifestUnsupportedVersion`, `ManifestTooManyNodes`, `ManifestDuplicateId`, `StyleInvalid`,
    `StyleUnreadable`, `StyleTooLarge`).
  - The stdio `headless` serve lane takes `--app <tessera.json>` for the same manifest scene (the
    flag is rejected together with `--script`, which names `app.config` itself). Not yet: snapshots
    do not add manifest names/paths to nodes.

## 3. CLI

```bash
tessera test --script tests/agent/login.json --report out/report.json
tessera test --script tests/agent/scroll.json --mode golden --out out/
tessera headless --mode audit --profile desktop --script tests/audit/home.json
```

Exit codes: `0` pass, `1` assertion/audit failure, `2` invalid script/config.

Status: `tessera test --script FILE` and `tessera headless --mode structural|audit --script FILE`
run the script against the built-in windowless fixture (section 6.8) or, when the script names
`app.config`, the manifest scene (section 2), write `out/report.json`, and
return the section 3 exit codes. `--mode golden|gpu-smoke` run the same script, then render the final scene offscreen and compare it
with `--baseline <png>` (`--update-goldens` rewrites it from the software path only; `--font <ttf>`
overrides the repository test font). Exit `0` match, `1` mismatch or missing baseline, `2` the mode
cannot run here (golden needs lavapipe via `VK_ICD_FILENAMES`, ADR-009). The report gains a `frame`
object and the sink gets `frame.png`, the diff image and `frame.compare.json`. Transient input state
(hover, focus ring) is not replayed into the capture. `--help` and `--version` work. See section 6.5.

## 4. Script schema

```json
{
  "scriptVersion": 1,
  "profile": { "width": 1280, "height": 800, "scale": 1.0, "locale": "en-US" },
  "app": { "config": "tessera.json" },
  "steps": [
    { "action": "launch" },
    { "action": "waitFor", "uiId": "987654321098765432", "state": "visible", "timeoutMs": 1000 },
    { "action": "type", "uiId": "111111111111111111", "text": "alice" },
    { "action": "click", "uiId": "222222222222222222" },
    { "action": "assert", "uiId": "333333333333333333", "text": "Welcome, alice" },
    { "action": "snapshot", "path": "out/after-login.json" },
    { "action": "screenshot", "path": "out/after-login.png" },
    { "action": "trace", "path": "out/trace.json" },
    { "action": "audit", "profile": "desktop", "path": "out/audit.json" }
  ]
}
```

Actions: `launch`, `close`, `resize`, `click`, `type`, `key`, `scroll`, `focus`, `waitFor`,
`assert`, `snapshot`, `screenshot`, `trace`, `audit`, `custom`, `assertCustom`, `navigate`,
`back`, `openOverlay`, `closeOverlay`, `assertRoute`, `assertText`, `assertSelection`.

Assertions:

- node exists/visible/enabled/focused/selected
- text/value/bounds/role/name
- list item exists `(uiId, itemKey)`
- event count/order
- no error-boundary catch/crash
- frame/metrics budgets
- golden image match
- audit score/threshold
- custom element state/draw stats/last event (`custom`/`assertCustom`)
- route/params, text/selection, overlay open/close/focus

### 4.1 Custom elements

- Target a custom element by `uiId`; sub-targets may use `partId` from `hit_test_part`.
- `custom` invokes a debug/headless-only `on_test_command`; `assertCustom` reads
  `CustomElementSnapshot` paths.
- Structural mode records passes/pipelines/draws without a GPU; golden mode renders offscreen
  with compiled shaders; GPU smoke validates a real device.
- Snapshots include element id, state, draw counts/pipeline names, metrics, and last event.
- `continuous` elements advance by the fixed frame script, never wall clock.
- See [`custom-elements-v1.md`](custom-elements-v1.md).

## 5. Outputs

```text
out/
├── report.json        # pass/fail, steps, metrics, diagnostics
├── snapshot.json      # UiSnapshot dump
├── frame.png          # screenshot (golden/structural optional)
├── trace.json         # Chrome/Perfetto-compatible spans
└── audit.json         # Lighthouse-style audit (optional)
```

`report.json` includes step index, action, target ID, result, duration, error, and artifact
paths. Failures include a diagnostic and a minimal repro script when one can be generated (which
failures cannot is not specified).

## 6. Agent protocol

For coding agents, the harness also exposes a line-delimited JSON protocol over stdin/stdout
(or a local socket):

The `tessera-mcp` stdio server exposes these commands as Model Context Protocol tools; it is a thin
adapter over this section and adds no second protocol
([`designer-studio-v1.md`](designer-studio-v1.md) section 8).

```json
{"id":1,"cmd":"launch","script":"tests/agent/home.json"}
{"id":2,"cmd":"snapshot"}
{"id":3,"cmd":"click","uiId":"222222222222222222"}
{"id":4,"cmd":"waitFor","uiId":"333333333333333333","state":"visible","timeoutMs":1000}
{"id":5,"cmd":"trace","path":"out/trace.json"}
{"id":6,"cmd":"audit","profile":"desktop"}
```

- Responses echo `id`, `ok`, `result`/`error`, and timing.
- Commands are serialized; no concurrent UI mutation.
- IDs are decimal strings (uint64).
- The protocol is local-only and authenticated by the harness process; no network exposure.
- A command timeout aborts the step and returns a diagnostic; the harness remains usable.

### 6.1 Codec (implemented)

`tessera_harness` (`include/tessera/harness/`, `src/harness/`) implements the transport-neutral half
of this section. It links no project or third-party library and contains no socket, token, or auth
code (the section 9 boundary).

- **Framing** (`line_framing.hpp`): one JSON document per line; a 1 MiB payload cap rejected with
  the typed `FrameErrorCode::LineTooLong`; LF and CRLF terminators; a final unterminated line at
  EOF; strict UTF-8 validation per line. A rejected line is dropped whole, so a malformed frame is
  never half-delivered. `json_parse.hpp` bounds nesting at 64 and rejects duplicate keys, raw
  control characters, lone UTF-16 surrogates, and integers outside int64.
- **Transport** (`line_transport.hpp`, `in_process_transport.hpp`): `LineTransport::read_line`
  returns one line, `WouldBlock` (peer still open, no complete line yet), or `Closed`, and never
  blocks. `write_line` rejects empty payloads, embedded CR/LF, over-cap lines, and invalid UTF-8.
  A transport with internal buffering exposes `flush()` and may report `QueueFull`. The in-process
  pair is the deterministic implementation for tests and embedding.
- **Envelope** (`protocol.hpp`): `protocolVersion` 1, `id` as a decimal string (a small JSON number
  is also accepted on input), `cmd`, and flat command arguments. A request without
  `protocolVersion` is v1; any other value is `unsupportedVersion`. Responses echo `id`, then carry
  `ok`, `result` or `error` (`code`/`message`/`detail`), and `elapsedUs`. Error codes are
  `malformedRequest`, `unsupportedVersion`, `unknownCommand`, `invalidArgument`, `uiIdNotFound`,
  `timeout`, `internal`; a failure envelope echoes a recovered `id` when the request had one.
- **Commands** (`command.hpp`, `command_result.hpp`): `snapshot` (`includeHidden`); `click`,
  `setValue` (`value`), `focus`, `scroll` (`dx`, `dy`), and `scrollIntoView`, all taking `uiId` (decimal string) and an
  optional `itemKey`; `trace` (`path`); `audit` (`profile`, optional `path`). Unknown command names
  and unknown parameters are rejected, not ignored. The snapshot result carries the canonical
  snapshot object from [`automation-ids-v1.md`](automation-ids-v1.md) section 5.2 as an embedded
  value; this slice does not compute or reinterpret UiIds.
- **Determinism** (`json_value.hpp`, `json_write.hpp`): object members keep wire order, output has
  no insignificant whitespace, integers stay exact, doubles use the shortest round-trip form, and
  the same value always serializes to the same bytes.

Still pending for the full section: the stdio adapter, the executor that applies commands to the
real runtime, and the `tessera test` / `tessera headless` CLIs.

### 6.2 Stdio transport (implemented)

`stdio_transport.hpp` adapts the framer to the process's stdin/stdout, or to any injected
`StdioStream`, and is the v1 transport.

- **`read_line` never waits.** The OS stream probes readiness with a zero-timeout `poll` (POSIX) or
  a zero-timeout wait/peek (Windows) and reports `wouldBlock` otherwise; the read path never flips a
  blocking handle into non-blocking mode. An interactive console in line-input mode may itself wait
  for a complete line, which is why the CLI is normally driven by a pipe; a pipe or a file never
  waits.
- **Bounded blocking wait.** `LineTransport::wait_readable(timeout)` is the only place a serve loop
  idles: it returns true when `read_line` can make progress (a buffered line, a framing error, EOF,
  or a closed transport) and false when `timeout` elapsed first. It never reads and never exceeds
  the timeout. POSIX uses `poll(2)` with the timeout. Windows cannot reliably wait on a pipe or
  file handle, so a pipe is peeked with `PeekNamedPipe` between `Sleep` slices of at most 2 ms (no
  reader thread, so nothing to join and no queue to bound), a console uses
  `WaitForSingleObject`, and a disk file is always ready. A partial buffered line is not ready, so
  a half-delivered frame cannot make the loop spin. Transports without an override (the in-process
  pair) report ready at once and are for single-threaded tests only.
- **Clean EOF.** A final unterminated line is delivered, then the transport reports `Closed`.
  Framing rejections (over-long, invalid UTF-8, empty) are typed and framing resumes at the next
  LF. A streaming rejection keeps its position: a line that arrived after the rejected one cannot
  overtake it.
- **Bounded writes.** A payload is validated and framed with one LF into a bounded queue (2 MiB by
  default, clamped to at least one maximum frame). `write_line` succeeds once the line is queued;
  a partial OS write keeps the remainder queued and `flush()` resumes it in order. A full queue
  reports `queueFull` and writes nothing, so retrying the same line cannot duplicate bytes.
  Writing to a peer that stopped draining stdout is not allowed to wait: on POSIX the write is
  issued with `O_NONBLOCK` raised for that one call and restored afterwards, so a full pipe reports
  `wouldBlock` and the frame stays queued. Windows cannot change an already-open synchronous pipe
  handle, so a peer that leaves the pipe full delays a `WriteFile` until it drains; the serve loop
  itself still only idles on `wait_readable`.
- No socket, token, or auth code: the protocol is local-only (section 9).

### 6.3 Session executor (implemented)

`session.hpp` is the single-threaded request loop: read -> decode -> dispatch -> encode -> write.

- One FIFO holds decoded requests and decode failures, so responses leave in request arrival
  order. A full request queue stops reading (back-pressure) instead of dropping work or growing.
- `pump()` is one bounded slice: at most `maxReadsPerPump` lines and `maxCommandsPerPump` items,
  sized so a host can call it from the existing `RuntimeLoop` tick. It never waits, spawns a
  task, or blocks. `pump(SessionTickBudget)` narrows both counts to one tick's work (a fixed-step
  host passes its `RuntimeLoop` update bound), so one pump can never outrun one tick.
- The per-command timeout uses simulated time from an injected `SessionClock`. A request whose
  deadline passed before dispatch is answered with `timeout`, and a handler that overruns the
  budget has its result replaced with `timeout`. A synchronous handler cannot be preempted,
  which is exactly why commands are serialized; the session stays usable after either case.
- `shutdown()` answers queued requests in arrival order with an `internal` cancellation
  diagnostic, writes as much as the transport accepts without waiting, then closes. Nothing to
  join exists because the loop owns no thread.
- A response larger than the transport's line cap is replaced with an `internal` (`lineCap`)
  failure, so an oversized snapshot cannot desynchronize the stream.

### 6.4 Command handler and artifacts (implemented)

`command_handler.hpp` is the dispatch seam; `harness_handler.hpp` implements v1:

- `snapshot` consumes the host's canonical automation snapshot object (`AutomationHost`) and
  embeds it unchanged. The harness never computes or rewrites UiIds.
- `click`, `setValue`, `focus`, and `scroll` go through the host's normal input path and are
  echoed in the result; they do not mutate state directly.
- `trace` and `audit` write the host document through `ArtifactSink` under the section 5 names
  (`trace.json`, `audit.json`, default directory `out/`), capped at 4 MiB. A path taken from a
  script must be relative, `/`-separated, and made only of `[A-Za-z0-9._-]` components; absolute,
  drive-qualified, backslash, `..`, dot, and empty components are `invalidArgument`.

### 6.5 CLI (implemented)

`tessera_harness_cli` is built by default (promoted out of `EXCLUDE_FROM_ALL`) and implements the
section 3 invocations: `tessera test --script FILE [--out DIR] [--report PATH] [--mode
structural|audit]`, plus the `headless` equivalent.

- `tessera test` requires `--script`. `tessera headless --mode <m>` without `--script` serves the
  stdio protocol (section 6.2) against the fixture host (or the `--app` manifest scene) until EOF on stdin, one `RuntimeLoop` tick
  per loop iteration, idling in `wait_readable` (`serve.hpp`; default slice 50 ms, which also
  bounds how fast a stop flag is observed). It exits `0` on EOF; a closed stdout ends the session
  the same way. The v1 protocol has no shutdown command, so EOF (or the host's stop flag) is the
  end of a session. The serve loop creates no thread. `--mode` accepts `structural` and `audit`
  (both run the script; `audit` also adds the section 6.10 latency gate and enables the
  diagnostics seams); `golden`/`gpu-smoke` add the section 3 frame check (see
  [`golden-tests-v1.md`](golden-tests-v1.md) section 1.2). `--baseline` is required and
  `--update-goldens` is rejected outside `golden`; `serve` does not render frames.
- One `RuntimeLoop` tick per pump: the driver advances a `ManualClock` by one fixed step, ticks the
  loop, then calls `ScriptRunner::pump()` once (hard guard `kMaxScriptSteps + 1`). Time comes from
  an injected tick clock (1/60 s per tick), never the wall clock.
- The report goes to `<out>/report.json` (`out/` by default) or `--report`, both validated with the
  section 6.4 path rules. Exit codes: `0` pass, `1` step/run failure (script and protocol codes
  printed), `2` unreadable/invalid script or config. A failing run still writes its report.

### 6.6 Script runner (implemented)

`script.hpp` owns the section 4 document and `script_runner.hpp` executes it against the same
`CommandHandler` seam as the line protocol.

- **Format.** One JSON object with a closed member set: required `scriptVersion` (exactly 1;
  missing is `malformedScript` and any other value `unsupportedVersion`), optional `profile`
  (`width`/`height` 1..16384, `scale` in (0, 8], `locale` at most 64 bytes), optional `app`
  (`config` is a safe relative path), and required `steps` (at most 256). A script is untrusted
  input: parsing is all-or-nothing, so a bad step rejects the document before the first host
  command runs.
- **Steps.** v1 executes `click`, `type`, `focus`, `scroll`, `scrollIntoView`, `waitFor`, `assert`, `snapshot`, `trace`, and `audit`.
  `type` is the section 4 name for the `setValue` command. `uiId` must be a canonical decimal
  string (the protocol's small-integer convenience is not accepted for script input) and `itemKey`
  an optional non-empty string of at most 256 bytes. `timeoutMs` is 1..60000. Unknown members are
  rejected, not ignored. `snapshot` takes optional `includeHidden` and `path`; `trace`/`audit` take
  an optional `path`, where empty means the section 5 default name; `audit` requires `profile`. `assert` takes `uiId`,
  optional `itemKey`, and at least one of boolean `exists`/`visible`/`enabled`/`focused`/`selected` or
  string `text`/`name`; `waitFor` takes `state` (default `visible`). Both run the `snapshot` command
  with hidden nodes included, so `visible:false` and `exists:false` differ, and a mismatch fails the
  run with `assertionFailed` (detail names the field). `waitFor` does not poll: the host settles
  after every action, so it is a one-shot check of an already-settled state. The
  other section 4 actions (lifecycle, `assertText`/`assertCustom`/`assertRoute`/`assertSelection`, `screenshot`, custom-element,
  navigation, and overlay actions) parse as `unsupportedAction` because `AutomationHost` does not
  expose the capabilities they need; they are never skipped silently.
- **Execution.** `ScriptRunner::pump()` makes at most one synchronous `CommandHandler::handle` call,
  so a host can drive it from a `RuntimeLoop` tick. Commands stay serialized; the runner owns no
  thread and never waits, sleeps, polls, or reads a wall clock. It fails fast at the first failing
  step, and the report keeps the steps that ran.
- **Bounds.** `ScriptBudget` caps `max_steps` (default 256), a per-step `step_timeout_us` (default
  5 s, narrowed by a step's `timeoutMs`), and an optional whole-run `run_timeout_us`. Time comes
  only from the injected `SessionClock`: a step whose deadline passed before dispatch is answered
  `stepTimeout` without a handler call, and a handler whose simulated elapsed time overruns the
  budget has its result replaced with `stepTimeout`, mirroring 6.3. `cancel()` stops at the next
  pump. Stable codes: `malformedScript`, `unsupportedVersion`, `unknownAction`,
  `unsupportedAction`, `assertionFailed`, `invalidStep`, `scriptTooLarge`, `stepCapExceeded`, `stepTimeout`,
  `runTimeout`, `cancelled`, `handlerError`, `artifactError`, and `internal`; a handler failure also
  carries its protocol error code.
- **Artifacts.** A `snapshot` step with a `path` writes the canonical snapshot object through the
  `ArtifactSink` under the 6.4 path/cap rules; without a path the document is embedded in that
  step's result. `trace` and `audit` report the path their handler wrote.
- **Report.** `encode_script_report` emits canonical JSON with `scriptVersion`, `passed`,
  `cancelled`, `elapsedUs`, `stepsRun`, `stepsTotal`, `steps`, and an optional run-level `error`.
  Each step carries `index`, `action`, `uiId`/`itemKey` when targeted, `ok`, `elapsedUs`, either
  `result` or an `error` object (script code, message/detail, `step`, and `protocolCode` when a
  command failed), and `artifact` when a file was written.

### 6.7 Fake HAL (test-only, implemented)

`tests/unit/harness/fake_hal.hpp` builds into `tessera_harness_testing`, which no shipping target
links. It provides the section 2 platform edges as deterministic doubles:

- `FakeWindow` implements `hal::Window`: normalized `WindowMetrics` (logical/physical pixels, DPI
  scale, insets, focus, occlusion, minimized), oldest-first `poll_events`, and scripted
  `resize`/`set_dpi_scale`/`set_focused`/`set_occluded`/`set_minimized`/`push_event`/`close`.
  A resize derives physical pixels from the logical size and DPI scale. `wait_for_events` never
  sleeps: it reports whether an event is already queued. The queue is capped at 64 events; the
  oldest is dropped and counted.
- `ManualClock` implements `SessionClock`: time starts at 0 (or an explicit start), moves only
  through `advance_us`/`advance_ms`, and saturates instead of wrapping, so runner budgets and step
  durations are reproducible.
- It opens no OS window, device, or swapchain and starts no thread.

### 6.8 Engine host (implemented)

`EngineAutomationHost` (`include/tessera/automation/engine_host.hpp`,
`src/harness/engine_host*.cpp`) is the concrete `AutomationHost` over the real engine: one
`ui::Node` scene root driven by an `app::AppHost` frame loop. It links `tessera_harness` but is a
separate target (`tessera_automation_host`), because the protocol library itself stays engine-free.

- **Snapshot.** The host walks the live scene into the automation snapshot builder
  ([`automation-ids-v1.md`](automation-ids-v1.md) section 5.2) and returns the canonical document
  decoded as a `JsonValue`: `uiId` from `Node::automation_id` (0 = not targetable), `componentId`
  `"0"` until the scene carries one, the element kind, a structural preorder `path`
  (`root/View[0]/Button[1]`), `aria-label` as `name`, the paint text, world-pixel `bounds`, and
  ancestor-propagated `visible`; `enabled`/`focused`/`selected` come from the `Disabled`/`Focus`/
  `Checked` pseudo-states. Caps: `kMaxSnapshotNodes` 65536, `kMaxSnapshotDepth` 64,
  `kMaxSnapshotPathBytes` 256; exceeding a cap is `internal`, never a truncated tree. `sequence`
  is a host-owned counter starting at 0.
- **Actions.** Click, type, focus and scroll go through the production input path, never direct
  state mutation. Click queues `PointerMove`/`PointerDown`/`PointerUp` at the target's center into
  the host's `ui::InputPump`, so hover/active, implicit capture, pointer focus and click synthesis
  are the window driver's code. Type focuses the target through `FocusManager` (a non-focusable
  target is `invalidArgument`) and queues committed `EditText` payloads in <=256-byte UTF-8 chunks.
  Scroll queues a `Wheel` event at the target's center with the pixel delta divided by
  `kPixelsPerWheelUnit`, so the scroll default action clamps like real wheel input. After an action
  the host runs at most one settle frame, and only when the input made a frame due.
- **Targets.** The first visible preorder node whose `automation_id` matches is the target; an
  absent or hidden target is `uiIdNotFound`. With `itemKey`, the target is the first visible
  preorder node carrying both that `uiId` and that item key (`ui::Node::item_key()`, the
  `(UiId, ItemKey)` identity of automation-ids-v1 section 1). Snapshots emit the key as canonical
  decimal text (`itemKey`, empty for non-items) in document order, bounded by the existing node,
  depth and path caps. Only realized items have scene nodes, so an unrealized item is not
  targetable: it, an unknown key, a key under the wrong `uiId` and a hidden item are all
  `uiIdNotFound` (detail `<uiId>/<itemKey>`; no new error code). A key that is not canonical decimal
  (sign, leading zero, whitespace, overflow, text) is `invalidArgument`. `scroll` with an `itemKey`
  wheels at that row's center (bubbling to its scroller). `scrollIntoView` (same `uiId`/`itemKey`
  targeting, no offsets) reveals the realized target through `ui::scroll_into_view`, adjusting every
  scrollable ancestor like a focus reveal; an already visible target is an idempotent success.
  Revealing an unrealized virtual item is not available: it is `uiIdNotFound`.
- **Trace and audit.** `AutomationDiagnostics` attaches the existing
  `diagnostics::TraceRecorder` and `diagnostics::MetricsRegistry`; `enabled` gates all of it.
  Enabled, each command records one instant span and `trace` returns `to_chrome_trace_json`; `audit`
  returns a structural report (three counter-derived checks plus the registry snapshot) scored
  `100 * passed / checks` and passing at >= 90. Disabled, the host calls neither seam, answers
  `trace` with the empty Chrome document (`[]`), and answers `audit` with an `internal` diagnostic
  instead of a fabricated score; the cost is one branch per command.

### 6.10 Latency audit (implemented)

`tessera headless --mode audit --profile NAME --script FILE` adds a latency gate on top of the
section 6.8 structural audit. It is the only mode that reads a real clock (section 7).

- **Clock.** Audit mode runs `ScriptRunner` on `SteadySessionClock` (monotonic); structural mode
  keeps the simulated tick clock, so its reports stay byte-identical. `run_cli_with_audit_clock`
  injects a fake clock for tests, so threshold pass/fail never depends on the wall clock.
- **Samples.** Each successful `click`/`type`/`focus`/`scroll` step contributes its elapsed
  microseconds (dispatch, input delivery, settle frame). A run with no sample fails with
  `no-samples` instead of passing vacuously.
- **Profile.** `--profile` (default `desktop`) is a built-in name or a path ending in `.json`.
  Built-ins (p50/p95/max step us): `desktop` 8000/16000/50000, `mobile` 16000/33000/100000, `ci`
  unchecked/250000/1000000. Custom document: `{"latencyProfileVersion":1,"name":"x",
  "p50StepUs":N,"p95StepUs":N,"maxStepUs":N}`; thresholds are optional positive integers, at least
  one is required, unknown members are rejected. An unknown profile is exit `2`.
- **Output.** `<out>/latency-audit.json` (`auditVersion`, `kind:"latency"`, `profile`, `passed`,
  `samples`, `thresholds`, nearest-rank `stats` min/p50/p95/max, `violations` with `id`/`limitUs`/
  `actualUs`, per-step `steps`) and the same object under `latencyAudit` in `report.json`. Any
  violation exits `1`; the report is still written.
- Tests: `harness_latency_audit`. The disabled-diagnostics invariant stays proven by
  `harness_diagnostics_cost`.

### 6.9 Built-in fixture and wired tests (implemented)

Until the app-config loader lands, `tessera test --script` runs against the deterministic fixture
scene in `include/tessera/automation/fixture_scene.hpp`: a card `View` (id `9001`) with a counter
`Text` (id `9002`) and a focusable `Button` (id `9003`) that bumps it, a focusable `TextInput`
(id `9004`) that mirrors committed text, and a scroll list (id `9005`) with one item (id `9006`) and three keyed rows sharing id `9010`
with item keys `101`, `102`, `103`.
A script written for another app fails with `uiIdNotFound` until config loading exists.

Wired coverage (`tests/unit/harness/`): `harness_engine_host` (every action against a real scene,
plus byte-identical reruns), `harness_engine_host_failure` (`unsupportedAction`/`unknownAction`,
`uiIdNotFound`, step cap, both timeouts, non-focusable/hidden targets), `harness_engine_host_items` (`(uiId, itemKey)` snapshot keys and
click/focus/scroll resolution, unknown/malformed/hidden keys),
`harness_diagnostics_cost` (a disabled seam matches a seam-free run byte for byte), and
`harness_cli_integration` (exit codes, artifacts, deterministic reruns).

## 7. Determinism

- Fixed clock/timestep, size/DPI/fonts/locale, assets, and event scripts.
- No wall clock, network, system fonts, or random IDs in structural/golden modes.
- Snapshot order stable; manifest IDs stable.
- Async completions applied at frame boundaries in deterministic order.
- Time policy (stated once; [`lighthouse-audit-v1.md`](lighthouse-audit-v1.md) refers here):
  structural and golden modes use simulation time only; audit mode alone may use real
  (monotonic) elapsed time for latency, runs a fixed scene/script, and never affects golden
  pixels.

## 8. CI

- Run structural tests on every PR (fast, no GPU).
- Run golden tests on lavapipe/software ([`golden-tests-v1.md`](golden-tests-v1.md), ADR-009).
- Run GPU smoke on runners that have a GPU/Vulkan device; never update goldens from it.
- Run audits on a fixed profile with thresholds; upload report/trace/screenshots.
- Fail on assertion, crash, error-boundary catch, audit threshold, or golden diff.

## 9. Security and privacy

- Harness is local-only; no arbitrary filesystem/network access from scripts.
- Screenshots/traces may contain user data; fixtures must be synthetic.
- Redact secrets/PII; no telemetry by default.
- Scripts are untrusted input: validate paths/actions/IDs; reject traversal/oversized payloads.

## 10. Tests of the harness

- Script parsing/validation and unknown actions.
- Script runner and fake HAL: a happy path over the handler seam, the step-count cap, a per-step
  budget overrun, a malformed step, cancellation, and scripted metrics/events/clock stepping.
- The wired lane (6.8/6.9): a script over a real scene, failure paths through the real host
  (`unsupportedAction`, `uiIdNotFound`, step cap, both timeouts), the disabled-diagnostics no-cost
  rule, and the promoted CLI's exit codes and artifacts.
- Stdio adapter rules: WouldBlock vs. EOF, a final unterminated line, partial writes and queued
  retry, the line cap, and typed framing rejections that keep their stream position.
- Session behavior over a fake transport: every command answered, decode errors, timeout before
  and during dispatch, request back-pressure, shutdown cancellation/order, and a stalled output
  stream that resumes in order.
- Deterministic repeated runs (same manifest/snapshot/pixels).
- Query/action through the real input path.
- Custom element unit/structural/golden/GPU-smoke paths; custom commands/asserts; part ids.
- Timeout/waitFor cancellation; harness remains usable.
- Report/trace/snapshot schema; uint64 as strings.
- Golden/GPU-smoke separation; no baseline updates from GPU.
- No network/system-font access; path traversal rejected.

## 11. Open questions

- Protocol transport and auth: resolved as stdin/stdout (or the in-process pair), local-only,
  with no token; authentication is a property of how the process was started (sections 6 and 9).
  A local socket remains possible but is not in v1.
- Snapshot size/compression for large trees.
- Parallel test isolation (multiple harness processes).
- Headless accessibility tree exposure.
