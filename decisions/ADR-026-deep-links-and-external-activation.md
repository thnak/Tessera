# ADR-026: Deep links and external activation

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-020](ADR-020-window-resize-and-lifecycle.md) (window resize, display scale, and lifecycle), [ADR-023](ADR-023-os-event-normalization-and-foldables.md) (OS event normalization and foldables), [ADR-025](ADR-025-error-boundaries.md) (error boundaries)

## Context

Users click links in browsers, open files, tap notifications, or share content into the app.
The OS may launch a new process, reuse an existing one, or open a new window. Custom URL schemes
are claimable by other apps; HTTPS App/Universal Links require associated-domain setup. Deep
links are untrusted input and can carry sensitive or destructive parameters.

Without a declared mechanism, platform manifests drift, routes are hardcoded, cold-start links
are lost, auth gates drop the pending link, and multiple instances/windows fight over focus.

## Decision

1. **All external activations normalize to `DeepLinkEvent`** (URL, file, command line,
   notification, share); UI code never sees raw intents.
2. **Developer declares** schemes, HTTPS hosts, route patterns, auth/confirm flags, file types,
   single-instance/open-in policy, and a web fallback in `tessera.json.deepLinks`.
3. **Canonical links are HTTPS App/Universal Links on mobile**, with custom schemes as
   fallback/desktop. A web fallback page is required when the app is absent.
4. **Single instance is the default.** Forward the activation to the existing process, focus
   the window, and route; `newWindow` is opt-in.
5. **Pending activation survives cold start and auth.** A link to an auth-required route is
   resumed after login; logout clears pending user-scoped links.
6. **Deep links are untrusted.** Allowlist scheme/host/route; size/encoding/path validation; no
   tokens/secrets in URLs; no shell execution; destructive actions require confirmation.
7. **Platform declarations are generated/validated from config** for the platforms the generator supports (the platform list is not stated in this ADR; see [`docs/specs/deep-linking-v1.md`](../docs/specs/deep-linking-v1.md)); manual
   manifests must match.
8. **Tests cover cold/warm start, single instance, auth, malformed/oversized links, file
   associations, browser fallback, and redaction.**

## Consequences

### Positive

- One declared source of truth for links/routes.
- Links work from a cold start or an existing window.
- Auth/confirmation gates are respected.
- Platform declarations cannot silently drift.
- Untrusted input is validated in one place.

### Negative / risks

- Platform setup remains OS-specific (registry, plist, manifest, desktop file).
- Custom schemes can be spoofed; HTTPS verification requires hosting files.
- Browser launch prompts are outside our control.
- Single-instance forwarding needs IPC (inter-process communication) per platform.
- Pending-link/auth state adds lifecycle complexity.

## Alternatives considered

- **Hardcode platform manifests + routes.** Rejected: drift and duplicated validation.
- **Custom scheme only.** Rejected: spoofable and poor mobile UX; HTTPS links preferred.
- **HTTPS only.** Rejected: desktop browsers don't hand HTTPS URLs to native apps without a
  protocol handler/custom scheme.
- **Always open a new instance/window.** Rejected: poor UX and focus fights.
- **No deep links.** Rejected: external activation is a core app workflow.

## Follow-up

- Implement `deepLinks` schema validation and platform declaration generation.
- Add single-instance IPC per platform.
- Add pending-link/auth integration.
- Add fuzz/security tests and real-browser/device tests.
- Document assetlinks/AASA generation.
