// SPDX-License-Identifier: MIT
//
// `FakeNet`: the deterministic in-tree back end (docs/specs/networking-v1.md sections 4 item 1 and
// 5; issue #197). Section 4 orders it first and calls it "the default in tests and headless runs",
// so it ships in the library rather than in `tests/unit`: a headless run and a demo app use it to
// produce a defined result without a network, and the contract suite uses it as the reference
// behaviour every real back end is compared against.
//
// **Determinism, stated precisely.** A scripted exchange carries a delay in milliseconds, and the
// delay is measured on the injected `Clock`, never on a wall clock and never with a sleep. The fake
// enqueues the exchange and a test advances a `ManualClock`, calls `deliver_due()`, then drains the
// completion queue. Time is therefore an argument, and "the request is still in flight" is a fact
// the test asserts rather than waits for.
//
// **What it reproduces.** Section 5 lists the required cases; the fake makes each one reachable
// with data alone:
//
//   * a scripted status, headers and body (`push_response`);
//   * an injected failure of any kind, `offline`, `dns`, `connect`, `tls`, `other`
//     (`push_error`), so the three error outcomes are distinguishable by construction;
//   * a delay, so "still pending" and the module's timeout are both expressible;
//   * an `offline` switch that overrides the script, which is section 3 item 6's "the platform
//     reports no connectivity";
//   * a redirect `Location`, because the module - not the fake - follows it (net_transport.hpp
//     contract 2);
//   * the size limit, enforced by refusing to hand back a body over `max_response_bytes`
//     (section 3 item 5), which is what a real back end must do to avoid buffering it.
//
// Every call it receives is recorded in `received()`, in order, so a test asserts the whole
// redirect chain - method rewrites, body drops and the dropped `Authorization` - instead of only
// the final result.

#pragma once

#include "tessera/appfw/net_transport.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::appfw {

// One scripted exchange.
struct FakeNetScript {
    // What the fake reports. `NetResult` is the same value the seam hands back, so a script can
    // express "a 500 is a response" and "a DNS failure is an error" without a second vocabulary.
    NetResult result;
    // How long after the call the exchange becomes due, on the injected clock.
    std::uint32_t delay_ms = 0;
};

class FakeNet final : public NetTransport {
public:
    // The queue is where every completion is posted (contract 1: never inline) and the clock is
    // what a delay is measured on. Both are borrowed and must outlive the fake.
    FakeNet(runtime::CompletionQueue& completions, runtime::Clock& clock);

    FakeNet(const FakeNet&) = delete;
    FakeNet& operator=(const FakeNet&) = delete;

    // --- scripting -------------------------------------------------------------------------

    // Appends one exchange. Exchanges are consumed in the order they were pushed; a call beyond the
    // script reports `unsupported`, which is how a test notices it asked for more than it scripted.
    void push(FakeNetScript script);
    void push_response(NetResponse response, std::uint32_t delay_ms = 0);
    // The convenient form: build the error from its kind and a message.
    void push_error(NetErrorKind kind, std::string message = {}, std::uint32_t delay_ms = 0,
                    std::int64_t os_code = 0);

    // Section 3 item 6: with connectivity reported absent, every pending and future call reports
    // `offline` - not `connect` - because the platform can tell them apart here.
    void set_offline(bool offline) noexcept;
    [[nodiscard]] bool offline() const noexcept {
        return offline_;
    }

    // Section 4's "a back end may be unavailable in a build".
    void set_available(bool available) noexcept;
    [[nodiscard]] bool available() const noexcept override;

    // --- driving ---------------------------------------------------------------------------

    // Delivers every call whose due instant is at or before `clock().now_seconds()`, in call order.
    // Delivering means *posting* the completion, so the caller must then drain the completion queue
    // - the two stages stay separate, exactly as the real seam separates them. Returns how many
    // were delivered.
    std::size_t deliver_due();

    // Drops the script and every pending call. A dropped call never completes; the module's own
    // cancellation/timeout path is what resolves a request whose back end went away.
    void clear();

    // --- observation -----------------------------------------------------------------------

    // Every request the fake was handed, in order: the whole redirect chain is visible, so a test
    // asserts the hops rather than inferring them from the final URL.
    [[nodiscard]] const std::vector<NetRequest>& received() const noexcept {
        return received_;
    }
    // Calls still in flight (started and not yet due).
    [[nodiscard]] std::size_t in_flight() const noexcept {
        return pending_.size();
    }
    // Attempts the fake observed as cancelled: one that arrived with an already-cancelled token,
    // plus one still outstanding whose token was cancelled after it started. That second case is
    // the one section 3 item 4 is about ("releases the connection"), so it is counted rather than
    // assumed.
    [[nodiscard]] std::size_t cancelled_attempts() const noexcept;
    // Bodies the fake refused to return because they exceeded the request's cap.
    [[nodiscard]] std::size_t too_large_refusals() const noexcept {
        return too_large_refusals_;
    }
    [[nodiscard]] std::size_t scripted() const noexcept {
        return script_.size();
    }

    [[nodiscard]] std::string_view name() const noexcept override;

    // `NetTransport`.
    bool start(const NetRequest& request, runtime::CancellationToken token,
               Completion on_done) override;

private:
    struct Pending {
        NetRequest request;
        runtime::CancellationToken token;
        Completion on_done;
        NetResult scripted;
        double due_seconds = 0.0;
    };

    // The result for one accepted call, computed at delivery: the platform-state overrides
    // (`offline`, cancellation, an exhausted script), then the section 3 item 5 cap.
    [[nodiscard]] NetResult resolve_result(const Pending& entry, const NetRequest& request);

    runtime::CompletionQueue* completions_ = nullptr;
    runtime::Clock* clock_ = nullptr;
    std::vector<FakeNetScript> script_;
    std::vector<Pending> pending_;
    std::vector<NetRequest> received_;
    std::size_t script_cursor_ = 0;
    std::size_t cancelled_attempts_ = 0;
    std::size_t too_large_refusals_ = 0;
    bool offline_ = false;
    bool available_ = true;
};

} // namespace tessera::appfw
