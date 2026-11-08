# ADR-031: Production tooling and distribution

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-012](ADR-012-static-linking.md) (static linking), [ADR-013](ADR-013-project-license-mit.md) (MIT license), [ADR-022](ADR-022-diagnostics-and-profiling.md) (diagnostics and profiling), [ADR-024](ADR-024-native-first-boundaries.md) (native-first boundaries), [ADR-027](ADR-027-agent-automation-and-audits.md) (agent automation and audits)

## Context

A framework is judged in production: can developers inspect/hot-reload, can users update safely,
and can crashes be diagnosed without leaking data? Tessera has diagnostics/headless automation
but no native devtools protocol, update/signing spec, or crash-report pipeline.

## Decision

1. **Native devtools**: local-only, authenticated JSON protocol (stdin/stdout or socket) for
   scene/style/layout/semantics/events/metrics/screenshots/traces, hot reload, and custom
   element test commands. No JS console; disabled in release by default.
2. **Hot reload**: incremental compile -> patch/reconcile; preserve state by component/key on a best-effort basis (as stated in [`docs/specs/devtools-protocol-v1.md`](../docs/specs/devtools-protocol-v1.md)); failed compile keeps the last good frame; full reload is the fallback. Dev-only.
3. **Updates/distribution**: SemVer (semantic versioning) + signed manifest, HTTPS, hash/signature verification,
   resumable download, atomic stage/apply, health check, rollback, channels; platform-native
   installers/store policies; code signing/notarization where the target platform's policy requires it.
4. **Crash reporting**: bounded out-of-process dumps + breadcrumbs, separate symbols/build ids,
   version/arch matching, opt-in upload, redaction, offline queue/rate limits, grouping.
5. All tooling is local/opt-in, bounded, redacted, and must not change rendering/golden output.

## Consequences

- Developers can debug the real native app, not only the Wasm (WebAssembly) designer.
- Update/crash surfaces require signing/key management, retention, and privacy policy.
- Devtools/hot reload must be excluded or disabled in release.
- Crash symbol/version pipelines become release requirements.

## Alternatives

- Wasm designer only: rejected (not the real app/OS/GPU).
- Remote debugging/network listener: rejected (attack surface).
- Unsigned/auto-download updates: rejected (supply-chain risk).
- Full-memory crash dumps/always-on upload: rejected (privacy/size).

## Follow-up

- Define protocol schema/versioning and CLI (command-line interface).
- Implement hot reload patch/reconcile and state preservation.
- Implement signed manifests/updater/rollback and platform packaging.
- Implement dump capture/symbols/upload/redaction and grouping.
- Add release/recovery tests on clean VMs.
