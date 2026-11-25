// SPDX-License-Identifier: MIT
//
// Reading a computed CSS length: the piece of style application that `layout_style.cpp` needs but
// that does not belong to it. `layout_style.cpp` applies every property Yoga understands; this is
// the one parser all of them share.
//
// **Why it is its own unit.** `layout_style.cpp` is at its source-size budget - issue #700 needed
// four lines and could not have them - and this block is self-contained: one small struct, one
// accessor and one parser. Moving it lets the next style slice add behaviour instead of splitting
// the file under pressure.

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include <yoga/Yoga.h>

namespace tessera::ui::layout {

// A parsed `width`/`height`/`margin`/... value. `Invalid` is what every caller treats as "no
// declaration", which is why an unparseable value and a missing one behave the same.
struct Length {
    enum class Kind : std::uint8_t {
        Invalid,
        Auto,
        Points,
        Percent,
    };

    Kind kind = Kind::Invalid;
    float value = 0.0f;
};

// The computed value of `name`, or null when the property was not declared.
[[nodiscard]] const std::string* computed_property(const style::ComputedStyle& style,
                                                   const char* name);

// Parses `text` as a length in the CSS units layout supports: `px`, `%`, `auto`, `em`/`rem`
// (against the viewport's font size) and the viewport units the shared table names (issue #691).
// Anything else is `Invalid`.
[[nodiscard]] Length parse_length(std::string_view text, const LayoutViewport& viewport);

// CSS size containment (issue #700): a query container whose `container-type` contains an axis
// sizes that axis from itself, not from its descendants, which keeps a self-referencing
// `@container` rule from changing the size that selected it. Content contributes zero - on the
// default `content-box` that is `width: 0`, so padding and border survive. Only an `auto` axis is
// touched: this removes a dependency on content, it never overrides an authored length.
void apply_size_containment(YGNodeRef node, const style::ComputedStyle& style,
                            const std::string* box_sizing);

} // namespace tessera::ui::layout
