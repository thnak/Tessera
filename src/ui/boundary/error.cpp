// SPDX-License-Identifier: MIT

#include "tessera/ui/boundary/error.hpp"

#include <string_view>
#include <utility>

namespace tessera::ui::boundary {
namespace {

// Cuts at a UTF-8 code-point boundary so a truncated message is still valid text.
[[nodiscard]] std::string truncate(std::string_view text, std::size_t cap) {
    if (text.size() <= cap) {
        return std::string(text);
    }
    std::size_t cut = cap;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return std::string(text.substr(0, cut));
}

} // namespace

std::string_view class_name(ErrorClass kind) {
    switch (kind) {
    case ErrorClass::Recoverable:
        return "recoverable";
    case ErrorClass::Cancellation:
        return "cancellation";
    case ErrorClass::Fatal:
        return "fatal";
    case ErrorClass::Platform:
        return "platform";
    }
    return "recoverable";
}

std::string_view site_name(FailureSite site) {
    switch (site) {
    case FailureSite::Build:
        return "build";
    case FailureSite::Update:
        return "update";
    case FailureSite::Effect:
        return "effect";
    case FailureSite::Cleanup:
        return "cleanup";
    case FailureSite::Event:
        return "event";
    case FailureSite::Style:
        return "style";
    case FailureSite::Layout:
        return "layout";
    case FailureSite::Paint:
        return "paint";
    }
    return "build";
}

const char* CancellationSignal::what() const noexcept {
    return "cancelled";
}

const char* PlatformFailure::what() const noexcept {
    return "platform failure";
}

Error bounded_error(Error error) {
    error.code = truncate(error.code, kMaxCodeBytes);
    error.message = truncate(error.message, kMaxMessageBytes);
    return error;
}

Error make_error(const std::exception& error) {
    Error result;
    result.code = "exception";
    result.message = error.what() != nullptr ? error.what() : std::string_view{};
    result.kind = ErrorClass::Recoverable;
    return bounded_error(std::move(result));
}

Error make_error(const std::bad_alloc& error) {
    Error result;
    result.code = "bad_alloc";
    result.message = error.what() != nullptr ? error.what() : std::string_view{};
    result.kind = ErrorClass::Fatal;
    return bounded_error(std::move(result));
}

Error make_error(const CancellationSignal& error) {
    Error result;
    result.code = "cancelled";
    result.message = error.what();
    result.kind = ErrorClass::Cancellation;
    return bounded_error(std::move(result));
}

Error make_error(const PlatformFailure& error) {
    Error result;
    result.code = "platform_failure";
    result.message = error.what();
    result.kind = ErrorClass::Platform;
    return bounded_error(std::move(result));
}

Error make_error() {
    Error result;
    result.code = "unknown_exception";
    result.message = "non-standard exception";
    result.kind = ErrorClass::Recoverable;
    return bounded_error(std::move(result));
}

} // namespace tessera::ui::boundary
