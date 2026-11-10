// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <string_view>

namespace tessera::ui::boundary {

// Caps so one report never grows without bound (docs/specs/error-boundaries-v1.md section 7).
inline constexpr std::size_t kMaxCodeBytes = 64;
inline constexpr std::size_t kMaxMessageBytes = 512;

// Who must handle a failure (section 1). Only `Recoverable` failures are caught by a component
// boundary; the other classes bypass boundaries so isolation never hides a process-level or
// platform failure behind a fallback.
enum class ErrorClass : std::uint8_t {
    Recoverable,
    Cancellation, // control outcome, not an error: no fallback, no error report
    Fatal,        // OOM/bad_alloc, stack overflow, abort, process-level failure
    Platform,     // HAL/GPU/device-loss failure outside the UI's control
};

[[nodiscard]] std::string_view class_name(ErrorClass kind);

// Where generated component code was running when it failed. `catchEvents` gates only `Event`, so
// strict debugging can opt one boundary into letting event-handler throws propagate.
enum class FailureSite : std::uint8_t {
    Build,
    Update,
    Effect,
    Cleanup,
    Event,
    Style,
    Layout,
    Paint,
};

[[nodiscard]] std::string_view site_name(FailureSite site);

[[nodiscard]] constexpr bool is_event_site(FailureSite site) {
    return site == FailureSite::Event;
}

// Error value. Exceptions never cross a boundary, a thread, or an FFI edge: the guard turns a catch
// into this value, and this value is what fallback selection, onError, and async reports carry.
struct Error {
    std::string code;    // stable short code, e.g. "bad_alloc" or "user::invalid_state"
    std::string message; // redacted by make_report, capped at kMaxMessageBytes
    ErrorClass kind = ErrorClass::Recoverable;
    std::uint64_t source_span = 0; // generated-source span id for diagnostics, 0 = none
};

// Marker exceptions. Classification uses the guard's catch order, never RTTI: a component throws
// one of these to opt out of boundary isolation, and the matching catch clause assigns the class.
class CancellationSignal final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override;
};

class PlatformFailure final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override;
};

// The `std::bad_alloc` overload is selected statically by the guard's catch chain, so OOM is
// classified Fatal without a typeid/dynamic_cast on the exception path.
[[nodiscard]] Error make_error(const std::exception& error);
[[nodiscard]] Error make_error(const std::bad_alloc& error);
[[nodiscard]] Error make_error(const CancellationSignal& error);
[[nodiscard]] Error make_error(const PlatformFailure& error);
[[nodiscard]] Error make_error(); // foreign or unknown exception (`catch (...)`)

// Truncates to kMaxCodeBytes/kMaxMessageBytes. Kept separate from redaction so a caller-built
// Error from a component is bounded at the same place as a caught one.
[[nodiscard]] Error bounded_error(Error error);

} // namespace tessera::ui::boundary
