// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/table_span.hpp"
#include "tessera/ui/scene/node.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace tessera::ui::layout {
namespace {

constexpr int kMaxSpan = 1000;

// A decimal integer in [0, 1000]; anything else is the default.
int parse_span(std::string_view text, int fallback) {
    int value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || stop != end || value < 0) {
        return fallback;
    }
    return value > kMaxSpan ? kMaxSpan : value;
}

} // namespace

void apply_table_spans(LayoutInput& input, const Node& node) {
    input.col_span = 1;
    input.row_span = 1;
    for (const Node::AriaAttribute& aria : node.aria_attributes()) {
        if (aria.name == "aria-colspan") {
            const int span = parse_span(aria.value, 1);
            input.col_span = static_cast<std::uint32_t>(span < 1 ? 1 : span);
        } else if (aria.name == "aria-rowspan") {
            input.row_span = static_cast<std::uint32_t>(parse_span(aria.value, 1));
        }
    }
}

} // namespace tessera::ui::layout
