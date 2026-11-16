// SPDX-License-Identifier: MIT
//
// The per-request state machine of the `net` module (docs/specs/networking-v1.md sections 2 and 3;
// issue #197), as a value type with no I/O, no time and no allocation.
//
// It is separate from `NetClient` on purpose: the transitions are the part the spec pins, so they
// are testable without a transport, a queue or a clock, and the client only decides *when* to call
// them. Every transition reports whether it was accepted; a transition on a terminal machine is
// refused rather than silently applied, which is how "exactly once" is enforced structurally rather
// than by remembering to check a flag at each call site.
//
//   Idle --begin_attempt--> Loading --accept--> Done      (any response, any status)
//                              |-----fail-----> Error     (no response)
//                              |--cancelled---> Error
//                              |--timed_out---> Error
//
// `Loading --begin_attempt--> Loading` is a followed redirect: the request is not finished, only
// one hop is, so the observable state does not flicker back to `Idle`. `attempts` therefore counts
// hops, which is what `NetRequestStatus::attempts` reports and what the redirect limit is counted
// in.

#pragma once

#include "tessera/appfw/net_types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace tessera::appfw {

enum class NetRequestState : std::uint8_t { Idle = 0, Loading, Done, Error };

[[nodiscard]] constexpr std::string_view net_request_state_name(NetRequestState state) noexcept {
    switch (state) {
    case NetRequestState::Idle:
        return "idle";
    case NetRequestState::Loading:
        return "loading";
    case NetRequestState::Done:
        return "done";
    case NetRequestState::Error:
        return "error";
    }
    return "idle";
}

// The named outcome of a terminal request. `HttpStatus` exists because section 2 refuses to
// collapse a 4xx/5xx into an error; the five failure categories are the ones a caller branches on.
enum class NetOutcome : std::uint8_t {
    None = 0,
    Ok,         // a 2xx response
    HttpStatus, // a response whose status is not 2xx: a value, not an error (section 2)
    Timeout,    // section 3 item 3
    Cancelled,  // section 3 item 4
    Transport,  // offline / dns / connect / tls: no response (section 3 item 6)
    Limit,      // too_large or too_many_redirects (section 3 items 2 and 5)
    Request,    // bad_url / unsupported / other: refused before or by the transport
};

[[nodiscard]] constexpr std::string_view net_outcome_name(NetOutcome outcome) noexcept {
    switch (outcome) {
    case NetOutcome::None:
        return "none";
    case NetOutcome::Ok:
        return "ok";
    case NetOutcome::HttpStatus:
        return "http_status";
    case NetOutcome::Timeout:
        return "timeout";
    case NetOutcome::Cancelled:
        return "cancelled";
    case NetOutcome::Transport:
        return "transport";
    case NetOutcome::Limit:
        return "limit";
    case NetOutcome::Request:
        return "request";
    }
    return "none";
}

// The category a `NetErrorKind` falls into: the mapping the state machine applies, exposed so a
// caller and a test agree on it without re-deriving it.
[[nodiscard]] constexpr NetOutcome outcome_of(NetErrorKind kind) noexcept {
    switch (kind) {
    case NetErrorKind::Cancelled:
        return NetOutcome::Cancelled;
    case NetErrorKind::Timeout:
        return NetOutcome::Timeout;
    case NetErrorKind::Offline:
    case NetErrorKind::Dns:
    case NetErrorKind::Connect:
    case NetErrorKind::Tls:
        return NetOutcome::Transport;
    case NetErrorKind::TooLarge:
    case NetErrorKind::TooManyRedirects:
        return NetOutcome::Limit;
    case NetErrorKind::BadUrl:
    case NetErrorKind::Unsupported:
    case NetErrorKind::Other:
        return NetOutcome::Request;
    }
    return NetOutcome::Request;
}

// Terminal outcome of a response: `Ok` for 2xx, `HttpStatus` otherwise (section 2).
[[nodiscard]] constexpr NetOutcome outcome_of_status(int status) noexcept {
    return status >= 200 && status <= 299 ? NetOutcome::Ok : NetOutcome::HttpStatus;
}

class NetStateMachine {
public:
    [[nodiscard]] NetRequestState state() const noexcept {
        return state_;
    }
    [[nodiscard]] NetOutcome outcome() const noexcept {
        return outcome_;
    }
    [[nodiscard]] std::uint32_t attempts() const noexcept {
        return attempts_;
    }
    [[nodiscard]] std::optional<NetErrorKind> error_kind() const noexcept {
        return error_kind_;
    }
    [[nodiscard]] int status() const noexcept {
        return status_;
    }
    // Done or Error. A terminal machine refuses every further transition.
    [[nodiscard]] bool finished() const noexcept {
        return state_ == NetRequestState::Done || state_ == NetRequestState::Error;
    }
    // Started and not terminal.
    [[nodiscard]] bool in_flight() const noexcept {
        return state_ == NetRequestState::Loading;
    }

    // Idle or Loading -> Loading, and one more hop. False when terminal (a stale transport callback
    // for a request a cancel or a timeout already resolved).
    bool begin_attempt() noexcept;

    // Loading -> Done. `status` is the response's; the outcome is `Ok` or `HttpStatus`, never an
    // error. False when terminal.
    bool accept(int status) noexcept;

    // Loading -> Error with the category derived from `error`. False when terminal.
    bool fail(const NetError& error) noexcept;

    // The two transitions the client drives from outside the transport: Loading -> Error with the
    // section-3 kinds the module itself decides (it measured the deadline, or the caller
    // cancelled).
    bool timed_out() noexcept;
    bool cancelled() noexcept;

private:
    // Every terminal transition goes through one place so "finished is final" cannot be forgotten.
    bool finish(NetRequestState next, NetOutcome outcome) noexcept;

    NetRequestState state_ = NetRequestState::Idle;
    NetOutcome outcome_ = NetOutcome::None;
    std::uint32_t attempts_ = 0;
    std::optional<NetErrorKind> error_kind_;
    int status_ = 0;
};

} // namespace tessera::appfw
