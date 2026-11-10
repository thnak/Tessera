// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/boundary/boundary.hpp"

#include <cstdint>
#include <utility>

namespace tessera::ui::boundary {

struct GuardOutcome {
    bool ran = false;     // the guarded code completed without throwing
    CatchOutcome failure; // how the failure was routed; meaningful only when `ran` is false
};

// Converts the exception currently being handled into a value. Call it from inside a catch handler;
// with no active exception this is a program error (std::terminate). It is the conversion step for
// an FFI/thread edge: a bridge returns Result<T, Error> instead of throwing across the ABI.
[[nodiscard]] Error capture_current_error();

// The one synchronous catch point for generated component code (spec section 5). It runs the
// guarded callable, converts every throw into an Error value, and hands routing to the boundary
// table, so the exception never crosses the boundary, a thread, or an FFI edge.
//
// Deliberately not `noexcept`: a throw inside a `noexcept` function terminates, and a failure of
// the catch point itself (OOM while routing) must reach the process handler instead.
template<typename Fn>
[[nodiscard]] GuardOutcome run_guarded(BoundaryTable& table, BoundaryId boundary, FailureSite site,
                                       double now_seconds, Fn&& fn, std::uint64_t source_span = 0) {
    try {
        std::forward<Fn>(fn)();
        return GuardOutcome{true, {}};
    } catch (...) {
        Error error = capture_current_error();
        error.source_span = source_span;
        return GuardOutcome{false,
                            table.catch_error(boundary, std::move(error), site, now_seconds)};
    }
}

} // namespace tessera::ui::boundary
