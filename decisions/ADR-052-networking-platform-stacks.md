# ADR-052: Networking uses the platform HTTP stacks behind one `fetch`-shaped interface

- Status: Accepted (2026-11-13, owner)
- Date: 2026-11-13
- Related: [ADR-012](ADR-012-static-linking.md) (static linking),
  [ADR-021](ADR-021-async-runtime.md) (async runtime),
  [ADR-050](ADR-050-opt-in-app-framework-layer.md) (app-framework layer),
  [`networking-v1.md`](../docs/specs/networking-v1.md) (the normative spec),
  [`app-framework-open-questions.md`](../docs/design/app-framework-open-questions.md) section 5
  (options and rationale), GitHub issue #197

## Context

The app framework needs an HTTPS client (issue #197). Three options were weighed in
`app-framework-open-questions.md` section 5: platform stacks (A), one vendored libcurl plus TLS
library (B), or an interface with community back ends only (C).

## Decision

1. **Option A.** The `net` module uses the platform stack on each platform: WinHTTP (Windows),
   NSURLSession (macOS, iOS), browser `fetch` (Wasm), the Android stack through JNI with the Android
   backend, and **libcurl as the Linux back end only**.
2. One `fetch`-shaped interface with no platform types and no socket API; behavior is pinned by a
   contract suite (redirects, errors, timeout, cancellation, size limit, no implicit cookie jar or
   cache). The suite runs against a deterministic `FakeNet` in normal CI and against a loopback
   server in an opt-in lane for the real back ends.
3. **WebSocket is not V1**; it is a separate interface later.
4. Order: `FakeNet`, WinHTTP, Linux libcurl, `fetch`, Apple, Android.
5. TLS stays with the OS wherever possible, so OS security updates reach apps without a rebuild.
   The Linux TLS library (mbedTLS or OpenSSL, both Apache-2.0) is chosen in the libcurl slice and
   needs entries in `docs/specs/dependencies.md` and `THIRD_PARTY_NOTICES.md` first.

## Consequences

- Behavior differs at the edges between platforms; the contract suite fixes the normative behavior
  and each back end configures its stack to match.
- HTTP/3 and proxy behavior on Linux follow libcurl and lag the platforms.
- Community users who want one stack everywhere can ship a libcurl back end as a Tier 2 provider
  ([ADR-049](ADR-049-backend-extensibility.md)).
- Revisit when the contract suite shows a platform difference that configuration cannot remove, or a
  platform deprecates its stack.
