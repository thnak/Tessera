// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_error.hpp"

namespace tessera::ui::a11y {

std::string_view to_string(UiaError error) {
    switch (error) {
    case UiaError::None:
        return "none";
    case UiaError::UnsupportedPlatform:
        return "unsupported-platform";
    case UiaError::ApartmentUnavailable:
        return "apartment-unavailable";
    case UiaError::ComFailure:
        return "com-failure";
    case UiaError::ThreadFailed:
        return "thread-failed";
    case UiaError::NotStarted:
        return "not-started";
    case UiaError::AlreadyStarted:
        return "already-started";
    case UiaError::NoSnapshot:
        return "no-snapshot";
    case UiaError::UnknownNode:
        return "unknown-node";
    case UiaError::PatternNotSupported:
        return "pattern-not-supported";
    case UiaError::ElementNotEnabled:
        return "element-not-enabled";
    case UiaError::ThreadViolation:
        return "thread-violation";
    case UiaError::QueueFull:
        return "queue-full";
    case UiaError::Timeout:
        return "timeout";
    case UiaError::OutOfMemory:
        return "out-of-memory";
    case UiaError::Internal:
        return "internal";
    }
    return "internal";
}

} // namespace tessera::ui::a11y
