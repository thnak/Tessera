// SPDX-License-Identifier: MIT

#include "tessera/ui/responsive/size_class.hpp"

#include <cmath>

namespace tessera::ui::responsive {

SizeClass size_class_for(double logical_width) noexcept {
    // A window that has not been measured yet (or a caller that passed a nonsense width) is the
    // smallest class: a component that branches on the class then renders its narrow layout, which
    // is the safe direction for a first frame.
    if (!std::isfinite(logical_width) || logical_width < 0.0) {
        return SizeClass::Compact;
    }
    if (logical_width < kCompactMaxWidth) {
        return SizeClass::Compact;
    }
    if (logical_width < kMediumMaxWidth) {
        return SizeClass::Medium;
    }
    return SizeClass::Expanded;
}

SizeClass size_class_for(const style::MediaContext& context) noexcept {
    return size_class_for(context.viewport_width);
}

std::string_view size_class_name(SizeClass value) noexcept {
    switch (value) {
    case SizeClass::Compact:
        return "compact";
    case SizeClass::Medium:
        return "medium";
    case SizeClass::Expanded:
        return "expanded";
    }
    return "compact";
}

std::optional<SizeClass> parse_size_class(std::string_view name) noexcept {
    if (name == "compact") {
        return SizeClass::Compact;
    }
    if (name == "medium") {
        return SizeClass::Medium;
    }
    if (name == "expanded") {
        return SizeClass::Expanded;
    }
    return std::nullopt;
}

} // namespace tessera::ui::responsive
