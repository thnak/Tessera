// SPDX-License-Identifier: MIT
//
// `FakeNet` (docs/specs/networking-v1.md sections 4 item 1 and 5; issue #197). See net_fake.hpp for
// why the fake is a library type rather than a test fixture, and for the determinism contract.
//
// Two stages, like the real seam: `start` records the call and posts nothing; `deliver_due` posts
// the completion once the injected clock has reached the exchange's due instant. A test therefore
// moves time explicitly and drains the queue explicitly, and "still in flight" is a fact rather
// than a race.

#include "tessera/appfw/net_fake.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::appfw {

FakeNet::FakeNet(runtime::CompletionQueue& completions, runtime::Clock& clock)
    : completions_(&completions),
      clock_(&clock) {}

void FakeNet::push(FakeNetScript script) {
    script_.push_back(std::move(script));
}

void FakeNet::push_response(NetResponse response, std::uint32_t delay_ms) {
    push(FakeNetScript{NetResult{std::move(response)}, delay_ms});
}

void FakeNet::push_error(NetErrorKind kind, std::string message, std::uint32_t delay_ms,
                         std::int64_t os_code) {
    push(FakeNetScript{NetResult{std::unexpected(NetError{kind, std::move(message), os_code})},
                       delay_ms});
}

void FakeNet::set_offline(bool offline) noexcept {
    offline_ = offline;
}

void FakeNet::set_available(bool available) noexcept {
    available_ = available;
}

bool FakeNet::available() const noexcept {
    return available_;
}

std::string_view FakeNet::name() const noexcept {
    return "fake";
}

bool FakeNet::start(const NetRequest& request, runtime::CancellationToken token,
                    Completion on_done) {
    // Every call is recorded, including one that arrives already cancelled: `received()` is the
    // redirect chain a test asserts, and a hop that arrived cancelled is part of that chain.
    received_.push_back(request);
    const bool cancelled = token.cancelled();
    if (cancelled) {
        ++cancelled_attempts_;
    }
    Pending entry;
    entry.request = request;
    entry.token = std::move(token);
    entry.on_done = std::move(on_done);
    // The script entry is consumed at `start`, not at delivery, so the script order is the call
    // order even when delays make the due instants disagree. The value is held by copy: the
    // exchange computes its result at delivery from this, so a cancellation or an offline flip in
    // the meantime is observed.
    entry.scripted = script_cursor_ < script_.size()
                         ? script_[script_cursor_].result
                         : NetResult{std::unexpected(NetError{NetErrorKind::Unsupported,
                                                              "the fake script is exhausted", 0})};
    const std::uint32_t delay_ms =
        script_cursor_ < script_.size() ? script_[script_cursor_].delay_ms : 0u;
    ++script_cursor_;
    entry.due_seconds = clock_->now_seconds() + static_cast<double>(delay_ms) / 1000.0;
    pending_.push_back(std::move(entry));
    return true;
}

std::size_t FakeNet::cancelled_attempts() const noexcept {
    // Both cases count: an attempt handed over with an already-cancelled token, and one still
    // outstanding whose token was cancelled after it started (which is the "releases the
    // connection" half of section 3 item 4).
    std::size_t outstanding = 0;
    for (const Pending& entry : pending_) {
        if (entry.token.cancelled()) {
            ++outstanding;
        }
    }
    return cancelled_attempts_ + outstanding;
}

NetResult FakeNet::resolve_result(const Pending& entry, const NetRequest& request) {
    if (offline_) {
        // Section 3 item 6: with connectivity reported absent the result is `offline`, not
        // `connect`, because this back end can tell the two apart.
        return std::unexpected(
            NetError{NetErrorKind::Offline, "the platform reports no connectivity", 0});
    }
    if (entry.token.cancelled()) {
        return std::unexpected(NetError{NetErrorKind::Cancelled, "the request was cancelled", 0});
    }
    if (!entry.scripted.has_value()) {
        return entry.scripted;
    }
    NetResponse response = *entry.scripted;
    // Section 3 item 5: a body larger than the cap is `too_large` and the bytes read so far are
    // discarded. A real back end stops reading; the fake refuses to hand the buffer over, which is
    // the same observable outcome without pretending to be a socket.
    if (request.max_response_bytes != 0 && response.body.size() > request.max_response_bytes) {
        ++too_large_refusals_;
        return std::unexpected(
            NetError{NetErrorKind::TooLarge, "the response body exceeds max_response_bytes", 0});
    }
    if (response.final_url.empty()) {
        response.final_url = request.url;
    }
    return NetResult{std::move(response)};
}

std::size_t FakeNet::deliver_due() {
    if (pending_.empty()) {
        return 0;
    }
    const double now = clock_->now_seconds();
    std::size_t delivered = 0;
    std::vector<Pending> remaining;
    remaining.reserve(pending_.size());
    for (Pending& entry : pending_) {
        if (entry.due_seconds > now) {
            remaining.push_back(std::move(entry));
            continue;
        }
        ++delivered;
        if (!entry.on_done) {
            continue;
        }
        // The result is computed at delivery, not at `start`: a token cancelled while the exchange
        // was in flight is observed here, and an offline switch flipped meanwhile applies to this
        // delivery, exactly as a platform stack would.
        NetResult result = resolve_result(entry, entry.request);
        // Posted, never called inline: the seam's contract 1, and what makes the UI thread the only
        // place a completion observes state.
        completions_->post(runtime::CompletionPriority::Visible,
                           [done = std::move(entry.on_done), result = std::move(result)]() mutable {
                               done(std::move(result));
                           });
    }
    pending_ = std::move(remaining);
    return delivered;
}

void FakeNet::clear() {
    pending_.clear();
    script_.clear();
    received_.clear();
    script_cursor_ = 0;
}

} // namespace tessera::appfw
