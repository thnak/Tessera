// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/table_span.hpp"
#include "tessera/ui/scene/node.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace tessera::ui::layout {
namespace {

// What the packed `LayoutInput` fields hold (layout_engine.hpp); a larger authored span clamps.
constexpr int kMaxColSpan = 255;
constexpr int kMaxRowSpan = 127;

// A decimal integer in [0, max]; anything else is the default.
int parse_span(std::string_view text, int fallback, int max_span) {
    int value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || stop != end || value < 0) {
        return fallback;
    }
    return value > max_span ? max_span : value;
}

} // namespace

void apply_table_spans(LayoutInput& input, const Node& node) {
    input.col_span = 1;
    input.row_span = 1;
    for (const Node::AriaAttribute& aria : node.aria_attributes()) {
        if (aria.name == "aria-colspan") {
            const int span = parse_span(aria.value, 1, kMaxColSpan);
            input.col_span = static_cast<std::uint32_t>(span < 1 ? 1 : span);
        } else if (aria.name == "aria-rowspan") {
            input.row_span = static_cast<std::uint32_t>(parse_span(aria.value, 1, kMaxRowSpan));
        }
    }
}

} // namespace tessera::ui::layout
