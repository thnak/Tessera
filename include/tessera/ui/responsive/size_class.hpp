// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/media_query.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace tessera::ui::responsive {

// The window size classes of docs/design/responsive.md section 3 and
// docs/specs/jsx-subset-v1.md section 7.1: the coarse buckets `useBreakpoint()` returns and the
// stylesheet's breakpoint tokens are written against.
//
// The thresholds are **named constants, not magic numbers**, because three consumers must agree on
// them: the hook, the preview's `viewport.set` report, and any app-authored CSS that writes the
// same numbers. They are fixed in V1 (issue #692 decision): an app overrides them by writing its
// own
// `@media` rules, not by reconfiguring the hook, so a stylesheet breakpoint and the hook can never
// disagree about where a class starts.
//
// **Header-only on purpose.** The preview's session and every canvas test target compile the
// session sources directly rather than linking a library, so a function with an out-of-line
// definition would need `tessera_responsive` on a dozen link lines. The whole computation is two
// comparisons, so inlining it costs nothing and keeps the dependency graph honest.
inline constexpr double kCompactMaxWidth = 600.0; // compact is strictly below this
inline constexpr double kMediumMaxWidth = 840.0;  // medium is [600, 840); expanded is 840 and up

enum class SizeClass : std::uint8_t {
    Compact,
    Medium,
    Expanded,
};

// The class of a logical window width in dp. The comparison is on the width alone: a size class
// describes the window, never the display, and never the height (a phone in landscape is still
// compact by width, which is what every browser's breakpoint does).
//
// A non-finite or negative width is `Compact`: a window that has not been measured yet is the
// smallest class, so a component that branches on the class renders its narrow layout rather than
// its widest one.
[[nodiscard]] inline SizeClass size_class_for(double logical_width) noexcept {
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

// The same answer from the media context the `@media` evaluator reads, so a stylesheet breakpoint
// and the hook cannot disagree: both take the width from `MediaContext::viewport_width`.
[[nodiscard]] inline SizeClass size_class_for(const style::MediaContext& context) noexcept {
    return size_class_for(context.viewport_width);
}

// The TSX spelling (`"compact"`, `"medium"`, `"expanded"`), which is also the IR string value.
[[nodiscard]] inline std::string_view size_class_name(SizeClass value) noexcept {
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

// Parses the TSX spelling; nullopt for anything else.
[[nodiscard]] inline std::optional<SizeClass> parse_size_class(std::string_view name) noexcept {
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
