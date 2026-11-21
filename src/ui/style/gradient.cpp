// SPDX-License-Identifier: MIT

#include "gradient_bodies.hpp"
#include "gradient_internal.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace tessera::ui::style {
namespace {

using detail::lower;
using detail::trim;

// The four function names this grammar knows, longest first so `repeating-linear-gradient(` is not
// matched as `linear-gradient(`.
struct FunctionName {
    std::string_view text;
    GradientValue::Kind kind;
    bool repeating;
};

constexpr FunctionName kFunctionNames[] = {
    {"repeating-linear-gradient(", GradientValue::Kind::Linear, true},
    {"repeating-radial-gradient(", GradientValue::Kind::Radial, true},
    {"linear-gradient(", GradientValue::Kind::Linear, false},
    {"radial-gradient(", GradientValue::Kind::Radial, false},
    {"repeating-conic-gradient(", GradientValue::Kind::Conic, true},
    {"conic-gradient(", GradientValue::Kind::Conic, false},
};

// Matches `text` against one function name and returns the text between its parentheses. The value
// must end with the matching `)`.
std::optional<std::string_view> body_of(std::string_view text, std::string_view head) {
    if (text.size() <= head.size() || text.back() != ')') {
        return std::nullopt;
    }
    if (text.substr(0, head.size()) != head) {
        return std::nullopt;
    }
    return text.substr(head.size(), text.size() - head.size() - 1);
}

} // namespace

bool is_gradient_function(std::string_view text) {
    const std::string value = lower(trim(text));
    for (const FunctionName& name : kFunctionNames) {
        if (value.rfind(name.text, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::size_t expand_repeating_stops(GradientStopValue* stops, std::size_t count) {
    // The destination holds kMaxRepeatingStops entries, so a longer authored ramp cannot be copied
    // into the cycle buffer; it is truncated to what fits rather than read past the end.
    count = std::min(count, kMaxRepeatingStops);
    if (count < 2) {
        return count;
    }
    // The authored ramp is one cycle: its period is the span from the first to the last offset. A
    // zero or negative period cannot repeat, so the ramp is left as authored.
    const float period = stops[count - 1].offset - stops[0].offset;
    if (!(period > 0.0f)) {
        return count;
    }
    // The cycle is repeated forwards and backwards from the authored one until it spans 0..1. The
    // authored ramp is copied first, because the expansion writes over the same array.
    std::array<GradientStopValue, kMaxRepeatingStops> cycle{};
    for (std::size_t index = 0; index < count; ++index) {
        cycle[index] = stops[index];
    }
    const float base = cycle[0].offset;
    // How many whole cycles fit before the authored one starts, and after it ends.
    const int before = static_cast<int>((base - 0.0f) / period);
    const int after = static_cast<int>((1.0f - cycle[count - 1].offset) / period);
    std::size_t written = 0;
    const auto emit_cycle = [&](int shift) {
        for (std::size_t index = 0; index < count && written < kMaxRepeatingStops; ++index) {
            GradientStopValue stop = cycle[index];
            stop.offset += static_cast<float>(shift) * period;
            stops[written++] = stop;
        }
    };
    for (int shift = -before; shift <= after && written < kMaxRepeatingStops; ++shift) {
        emit_cycle(shift);
    }
    return written;
}

std::optional<GradientValue> parse_gradient(std::string_view text) {
    const std::string value = lower(trim(text));
    for (const FunctionName& name : kFunctionNames) {
        const std::optional<std::string_view> body = body_of(value, name.text);
        if (!body) {
            continue;
        }
        GradientValue gradient;
        gradient.kind = name.kind;
        gradient.repeating = name.repeating;
        // The three shapes share one union slot (issue #731), so the active member must be
        // constructed before the body parser writes into it: a union member's default member
        // initializers do not run when it is not the one the constructor activated.
        if (name.kind == GradientValue::Kind::Linear) {
            gradient.linear = LinearGradientValue{};
            if (!detail::parse_linear_body(*body, gradient.linear)) {
                return std::nullopt;
            }
        } else if (name.kind == GradientValue::Kind::Radial) {
            gradient.radial = RadialGradientValue{};
            if (!detail::parse_radial_body(*body, gradient.radial)) {
                return std::nullopt;
            }
        } else {
            gradient.conic = ConicGradientValue{};
            if (!detail::parse_conic_body(*body, gradient.conic)) {
                return std::nullopt;
            }
        }
        return gradient;
    }
    return std::nullopt;
}

std::optional<LinearGradientValue> parse_linear_gradient(std::string_view text) {
    const std::optional<GradientValue> gradient = parse_gradient(text);
    if (!gradient || gradient->kind != GradientValue::Kind::Linear) {
        return std::nullopt;
    }
    return gradient->linear;
}

} // namespace tessera::ui::style
