// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>

namespace tessera::ui::a11y {

// Errors are values: no exception crosses the bridge-thread or FFI boundary (ADR-021, ADR-025).
enum class UiaError : std::uint8_t {
    None = 0,
    UnsupportedPlatform,  // this OS has no UIA apartment; accessibility stays off with a diagnostic
    ApartmentUnavailable, // COM refused the MTA (the thread already lives in an STA)
    ComFailure,           // a COM initialization call failed for another reason
    ThreadFailed,         // the bridge thread could not be created
    NotStarted,           // the bridge is not running (never started, stopped, or failed)
    AlreadyStarted,       // `start` while the worker is still alive
    NoSnapshot,           // no snapshot has been published yet
    UnknownNode,          // an index outside the current snapshot
    PatternNotSupported,  // the node's role/actions do not expose that UIA pattern
    ElementNotEnabled,    // a disabled node rejected an action
    ThreadViolation,      // a bridge-thread-only call arrived from another thread
    QueueFull,            // the bounded action/job ring is full
    Timeout,              // a bounded wait expired
    OutOfMemory,          // an allocation failed at the boundary
    Internal,             // defensive: an impossible state, reported not thrown
};

[[nodiscard]] std::string_view to_string(UiaError error);

} // namespace tessera::ui::a11y
