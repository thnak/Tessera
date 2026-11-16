// SPDX-License-Identifier: MIT
//
// The transport seam for the `net` module (docs/specs/networking-v1.md sections 2, 3 and 4; ADR-052
// items 1-2; issue #197).
//
// This header is the *whole* platform boundary. A back end is one class that turns a `NetRequest`
// into a `NetResult`; it names no socket, no handle and no platform type in its interface, which is
// ADR-052 item 2's "no platform types appear in the interface" enforced by the type system rather
// than by review. Section 4's order - `FakeNet`, WinHTTP, libcurl, `fetch`, NSURLSession, Android -
// is a list of subclasses of this class, one slice each.
//
// **What the seam owns, and what the module owns.** This split is the reason the contract suite can
// pin behaviour once instead of per platform.
//
// | Rule (section 3) | Owner | Why |
// |---|---|---|
// | item 1, no blocking on the calling thread | back end + `IoTaskExecutor` | only the back end
// knows what block it would do | | item 2, redirects | **module** (`net_redirect.hpp`) | the
// platforms disagree on every clause; see that header | | item 3, timeout | **module** (timer
// queue, injected clock) | the deadline must come from the injected clock | | item 4, cancellation
// | both | the module resolves `cancelled`; the back end observes the token and releases the
// connection | | item 5, size limit | back end (it is the only one that can stop buffering) and
// module (backstop) | a back end that forgets is caught | | item 6, offline | back end | only the
// platform can tell `offline` from `connect` | | item 7, headers | module adds `User-Agent`; back
// end adds nothing | "sets no ... header the caller did not ask for" | | item 8, TLS | back end |
// the OS trust store is the point of ADR-052 | | item 9, redaction | module (`redacted_url`) | one
// rule, one spelling | | item 10, determinism | module + back end | no wall clock in the interface
// layer |
//
// **The contract every back end keeps.**
//
//   1. `start()` returns at once and never runs `on_done` on its own stack - not even for a failure
//      it already knows. The completion is posted through the injected `CompletionQueue`, so the
//      app's callback runs on the UI thread.
//   2. It follows **no** redirects. A 3xx is a normal `NetResult` the module inspects. Every
//      platform stack must be configured that way (`WINHTTP_OPTION_REDIRECT_POLICY_NEVER`,
//      `CURLOPT_FOLLOWLOCATION 0`, `redirect: "manual"`).
//   3. It observes the `CancellationToken`: a cancelled attempt is dropped, its connection
//      released, and it may report `cancelled` or nothing at all. Reporting nothing is legal
//      because the module has already resolved the request; reporting `cancelled` late is also
//      legal because the module ignores a stale attempt.
//   4. It does not enforce the section 3 item 3 deadline. It may set a platform timeout as a safety
//      net, and a `timeout` it reports is delivered as `timeout`, but the normative deadline is the
//      module's, computed from the injected clock.
//   5. It adds no header, sets no cookie and reads no system proxy *setting* into the request: the
//      proxy the OS configuration names is the back end's business (ADR-052 item 5), but the
//      request's header list is the caller's.
//   6. It enforces `max_response_bytes` by stopping the read, and reports `too_large` with no body.
//
// `FakeNet` (`net_fake.hpp`) is the in-tree implementation section 4 orders first, and the one the
// contract suite runs against in normal CI.

#pragma once

#include "tessera/appfw/net_types.hpp"
#include "tessera/runtime/async.hpp"

#include <functional>
#include <string>
#include <utility>

namespace tessera::appfw {

class NetTransport {
public:
    // Exactly one call per accepted `start`, on the thread that drains the completion queue. It
    // takes the result by value: the back end has no further use for the response.
    using Completion = std::function<void(NetResult)>;

    virtual ~NetTransport() = default;

    NetTransport(const NetTransport&) = delete;
    NetTransport& operator=(const NetTransport&) = delete;

    // Starts one attempt. Returns false when the attempt did not start at all - the transport is at
    // its bound, or it cannot serve this request - and in that case `on_done` is never invoked.
    // `on_done` may still be dropped by a full completion queue after a `true` return, which is
    // `IoTaskExecutor`'s documented behaviour for the same reason (a bounded queue is a bound).
    virtual bool start(const NetRequest& request, runtime::CancellationToken token,
                       Completion on_done) = 0;

    // False when this build has no working back end for the platform, which section 4 turns into
    // `NetErrorKind::Unsupported` for every request instead of a failure at composition time.
    [[nodiscard]] virtual bool available() const noexcept = 0;

    // A short, stable name for diagnostics ("fake", "winhttp", "curl", "fetch"). Never a platform
    // type and never localised.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

protected:
    NetTransport() = default;
};

} // namespace tessera::appfw
