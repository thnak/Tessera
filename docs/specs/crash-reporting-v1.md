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

## 7. Open questions

- Crashpad vs platform-native handler per OS.
- Symbol server/retention and access control.
- Crash reporting opt-in UX and regulation.
- GPU device-loss reports vs app crashes.
- Breadcrumb schema/limits.
