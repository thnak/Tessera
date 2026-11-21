// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/media_query.hpp"

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
[[nodiscard]] SizeClass size_class_for(double logical_width) noexcept;

// The same answer from the media context the `@media` evaluator reads, so a stylesheet breakpoint
// and the hook cannot disagree: both take the width from `MediaContext::viewport_width`.
[[nodiscard]] SizeClass size_class_for(const style::MediaContext& context) noexcept;

// The TSX spelling (`"compact"`, `"medium"`, `"expanded"`), which is also the IR string value.
[[nodiscard]] std::string_view size_class_name(SizeClass value) noexcept;

// Parses the TSX spelling; nullopt for anything else.
[[nodiscard]] std::optional<SizeClass> parse_size_class(std::string_view name) noexcept;

} // namespace tessera::ui::responsive
