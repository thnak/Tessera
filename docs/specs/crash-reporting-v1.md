# Crash reporting and symbolication v1

Status: proposed (Big Step F, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision: [ADR-031](../../decisions/ADR-031-production-tooling-and-distribution.md) (devtools, updates/signing, crash reporting). Companion to
[`diagnostics.md`](../design/diagnostics.md), [`diagnostics-config-v1.md`](diagnostics-config-v1.md),
and [`security.md`](../design/security.md).

Crash dumps exist in diagnostics (config fields `crashDumps`, `crashDumpDir`, `breadcrumbs` in [`diagnostics-config-v1.md`](diagnostics-config-v1.md)); this spec covers the end-to-end report: capture, symbols,
privacy, upload, and version matching.

## 1. Goals

- Capture actionable crashes without blocking the UI or leaking PII (personally identifiable information).
- Symbolicate with the exact build symbols; group/dedupe reports.
- Opt-in/consent-aware upload; offline queue; bounded.
- Never execute crash data; never trust a dump.

## 2. Capture

- Crash handler writes a bounded minidump (Windows dump format) or core (Unix core dump) + metadata (version, commit, platform, GPU,
  config hash, feature flags, breadcrumbs).
- Breadcrumbs are a bounded ring (logs/traces/events); no secrets.
- Crashpad, Windows Error Reporting (WER), or Breakpad-style out-of-process handler where available (selection rule: G-24 in section 6.1; libraries listed in [`dependencies.md`](dependencies.md) section 8); no in-process unsafe code.
- Capture on fatal signals/exceptions/abort/GPU device loss (when the backend reports device loss).
- Do not capture full memory/user documents by default; allowlist regions only.
- Write atomically to a crash directory; survive restart; retry upload on the next launch (see the capture pipeline in section 6.1).

## 3. Symbols

- Build produces symbols (PDB on Windows, dSYM on macOS, ELF debug info on Linux) and a build id.
- Upload symbols separately from the app; never ship debug binaries.
- Server matches build id/version/arch; missing symbols are visible, not silently ignored.
- Symbolicate stack frames/registers; inline frames where supported (per-platform support is not specified).
- Keep symbols for all shipped versions; retention policy documented.

## 4. Upload and grouping

- Consent/opt-in; privacy policy; region/endpoint explicit.
- Queue offline; bounded retries/backoff; no crash-loop upload storm.
- TLS + auth; payload encrypted in transit; redact paths/user data.
- Group by signature (top frames/module/build), dedupe, rate-limit per app/version.
- Attach breadcrumbs/metrics/trace ids; no PII.
- Dashboard links to release/commit; alerting thresholds.

## 5. Privacy and security

- Redact user paths, window titles, clipboard, tokens, document content.
- Secure fields/text never captured.
- Validate dump metadata; treat uploads as untrusted.
- Retention/deletion policy; user can disable/delete.
- No executable content from reports; no auto-remediation scripts.

## 6. Testing

- Synthetic crash/minidump/abort/device loss; dump written and read back.
- Symbolication with known build; missing/mismatched symbols.
- Redaction fixtures (paths/tokens/secure text).
- Offline queue/retry/rate limit; corrupt dump rejected.
- Crash-loop -> rollback ([`updates-and-distribution-v1.md`](updates-and-distribution-v1.md)) or alert; version grouping.
- No UI-thread blocking on crash/upload.

## 6.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `CrashHandler` | signal/exception handler, dump write, metadata | Upload UI |
| `CrashReporter` | breadcrumbs, dump discovery, symbol/version metadata, queue | Logging/telemetry policy |
| `SymbolStore` | build-id lookup, symbol upload/retention | Release build |
| `CrashUploader` | consent, TLS/auth, retry/rate limit | Crash grouping UI |

Capture pipeline:

```text
fatal signal/exception/device-loss
  -> minimal async-signal-safe write (only operations that are safe inside a signal handler) (bounded dump + metadata)
  -> mark pending; do not allocate/lock/log in handler
  -> next launch: discover dump -> symbolicate/group metadata
  -> consent -> upload -> delete/retain per policy
```

Dump metadata:

- app/framework version, commit, build id, platform/arch, OS, GPU/driver, config hash.
- feature flags, locale, display mode, crash reason, thread/registers, module list.
- breadcrumbs ring (bounded, redacted), last trace/span ids, audit profile if enabled.
- No full heap/user documents by default; allowlisted memory regions only.

Symbols:

- Build produces PDB/dSYM/ELF debug + build id; upload separately from the app.
- Server matches `(build_id, version, arch)`; missing symbols are visible and reported.
- Keep symbols for all shipped versions; retention/access policy documented.
- Symbolication failure still uploads the dump/metadata; never block the app.

Upload/consent:

- Opt-in/consent; explicit endpoint/region; offline queue with bounded retries/backoff.
- Crash-loop/rate limit: cap reports per version/session; dedupe by signature.
- TLS/auth; payloads encrypted in transit; redact paths/titles/secrets.
- User can disable/delete reports; retention policy enforced.

Failure/edge cases:

- Corrupt/partial dump -> reject/diagnostic; no crash.
- Handler cannot write (disk full/permission) -> drop with a minimal log; never recurse.
- Upload service unavailable -> keep local queue bounded; never block startup.
- Symbol mismatch -> group by module/offset, mark "unsymbolicated".
- GPU device-loss vs app crash -> separate signatures/paths.

Gap-closure decisions (ids G-n are gaps and Q-n are open questions, listed in [`app-completeness-cross-check-v1.md`](../architecture/app-completeness-cross-check-v1.md); resolutions in [`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md)):

- **G-24/Q-23**: use Crashpad where available, with a platform-native handler fallback. Symbols
  live in an internal symbol store keyed by build id; upload is opt-in. Group by top 3 frames +
  build id; retention is configurable (default 90 days). Breadcrumbs are a bounded 256-entry
  redacted ring; GPU device-loss uses a separate signature.
- Crash reporting never blocks startup/first frame; upload queue is bounded/rate-limited.

## 6.2 Implemented: metadata model and exporter (v1 slice)

The report *payload* exists in `include/tessera/diagnostics/crash.hpp` +
`src/diagnostics/crash.cpp`; the platform capture in section 6.1 (handler, minidump, uploader,
symbol store) is still to come.

- `CrashMetadata` holds what a fatal-path handler can capture without allocating: version,
  commit, build id, platform/arch/OS, GPU, config hash, reason, thread, last trace id, an
  innermost-first frame list and an oldest-first breadcrumb ring. Every field is a view; an
  empty field is omitted from the document.
- `crash_metadata_json` writes canonical JSON (`"schema":"tessera.crash.v1"`). String values are
  JSON-escaped (control bytes as `\u00XX`) and truncated on a UTF-8 boundary, so a cap can never
  produce an invalid document or split a character.
- Caps: `max_frames` keeps the innermost frames, `max_breadcrumbs` keeps the newest breadcrumbs in
  their original order, `max_field_bytes`/`max_breadcrumb_bytes` cap single strings, and
  `max_text_bytes` bounds the two arrays (the fixed envelope adds at most 1 KiB). Every applied cap
  is reported in `"truncated":{"frames":N,"breadcrumbs":N,"text":bool}`; nothing is dropped
  silently.
- `redact_path` is the privacy rule for module names and breadcrumb paths: only the last component
  survives (`C:\Users\someone\app\main.cpp` -> `main.cpp`).
- `crash_signature` is the section 4 grouping key: build id plus the innermost
  `max_signature_depth` frames as `module!symbol+offset`; symbol names and module basenames only,
  so the key carries no path, address or user data.
- Determinism: no clock, no ASLR-dependent address (frames carry a module-relative offset) and
  caller-assigned breadcrumb sequences, so the same crash and build produce identical bytes and
  the golden tests are stable.
## 7. Deferred/open questions

- Public/self-hosted symbol server and access control.
- Consent UX/regulation per region.
- Breadcrumb schema versioning/limits.
- GPU device-loss detail beyond a separate signature.
- Mobile handler integration (deferred until the mobile backend exists, Big Step F).
