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

## 3. CLI

```bash
tessera test --script tests/agent/login.json --report out/report.json
tessera test --script tests/agent/scroll.json --mode golden --out out/
tessera headless --mode audit --profile desktop --script tests/audit/home.json
```

Exit codes: `0` pass, `1` assertion/audit failure, `2` invalid script/config.

Status: the CLI skeleton parses these commands and flags; `--help` and `--version` work. `--script`
is recognized but exits `2` with an explicit not-implemented diagnostic until the script runner
lands (see section 6.5).

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
  `setValue` (`value`), `focus`, and `scroll` (`dx`, `dy`), all taking `uiId` (decimal string) and an
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

- **Reads never wait.** The OS stream probes readiness with a zero-timeout `poll` (POSIX) or a
  zero-timeout wait/peek (Windows) and reports `wouldBlock` otherwise; it never flips a blocking
  handle into non-blocking mode. An interactive console in line-input mode may itself wait for a
  complete line, which is why the CLI is normally driven by a pipe; a pipe or a file never waits.
- **Clean EOF.** A final unterminated line is delivered, then the transport reports `Closed`.
  Framing rejections (over-long, invalid UTF-8, empty) are typed and framing resumes at the next
  LF. A streaming rejection keeps its position: a line that arrived after the rejected one cannot
  overtake it.
- **Bounded writes.** A payload is validated and framed with one LF into a bounded queue (2 MiB by
  default, clamped to at least one maximum frame). `write_line` succeeds once the line is queued;
  a partial OS write keeps the remainder queued and `flush()` resumes it in order. A full queue
  reports `queueFull` and writes nothing, so retrying the same line cannot duplicate bytes.
- No socket, token, or auth code: the protocol is local-only (section 9).

### 6.3 Session executor (implemented)

`session.hpp` is the single-threaded request loop: read -> decode -> dispatch -> encode -> write.

- One FIFO holds decoded requests and decode failures, so responses leave in request arrival
  order. A full request queue stops reading (back-pressure) instead of dropping work or growing.
- `pump()` is one bounded slice: at most `maxReadsPerPump` lines and `maxCommandsPerPump` items,
  sized so a host can call it from the existing `RuntimeLoop` tick. It never waits, spawns a
  task, or blocks.
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

### 6.5 CLI skeleton (implemented)

`tessera test` and `tessera headless` parse the documented commands, modes, and flags, and
`--help` / `--version` work. `--script` is recognized but rejected with exit code `2` and an
explicit not-implemented diagnostic until the script runner lands, as are a missing `--script`
for `test`, a missing `--mode` for `headless`, unknown modes or options, and empty option values.
The executable stays out of the default build (`EXCLUDE_FROM_ALL`) until that runner exists.

Still pending for the full section: the script runner behind `tessera test` / `tessera headless`,
the fake HAL, and the UI runtime wiring behind `AutomationHost`.

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
