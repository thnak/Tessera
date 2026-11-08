# Crash reporting and symbolication v1

Status: proposed (Big Step F). Decision: ADR-031. Companion to
[`diagnostics.md`](../design/diagnostics.md), [`diagnostics-config-v1.md`](diagnostics-config-v1.md),
and [`security.md`](../design/security.md).

Crash dumps exist in diagnostics; this spec covers the end-to-end report: capture, symbols,
privacy, upload, and version matching.

## 1. Goals

- Capture actionable crashes without blocking the UI or leaking PII.
- Symbolicate with the exact build symbols; group/dedupe reports.
- Opt-in/consent-aware upload; offline queue; bounded.
- Never execute crash data; never trust a dump.

## 2. Capture

- Crash handler writes a bounded minidump/core + metadata (version, commit, platform, GPU,
  config hash, feature flags, breadcrumbs).
- Breadcrumbs are a bounded ring (logs/traces/events); no secrets.
- Crashpad/Wer/breakpad-style out-of-process handler where available; no in-process unsafe code.
- Capture on fatal signals/exceptions/abort/GPU device loss (where applicable).
- Do not capture full memory/user documents by default; allowlist regions only.
- Write atomically to a crash directory; survive restart; retry upload later.

## 3. Symbols

- Build produces symbols (PDB/dSYM/ELF debug) and a build id.
- Upload symbols separately from the app; never ship debug binaries.
- Server matches build id/version/arch; missing symbols are visible, not silently ignored.
- Symbolicate stack frames/registers; inline frames where supported.
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
- Crash-loop -> rollback/alert; version grouping.
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
  -> minimal async-signal-safe write (bounded dump + metadata)
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

Gap-closure decisions:

- **G-24/Q-23**: use Crashpad where available, with a platform-native handler fallback. Symbols
  live in an internal symbol store keyed by build id; upload is opt-in. Group by top 3 frames +
  build id; retention is configurable (default 90 days). Breadcrumbs are a bounded 256-entry
  redacted ring; GPU device-loss uses a separate signature.
- Crash reporting never blocks startup/first frame; upload queue is bounded/rate-limited.

## 7. Deferred/open questions

- Public/self-hosted symbol server and access control.
- Consent UX/regulation per region.
- Breadcrumb schema versioning/limits.
- GPU device-loss detail beyond a separate signature.
- Mobile handler integration (deferred with the mobile backend).
