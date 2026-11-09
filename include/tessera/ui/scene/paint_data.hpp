// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace tessera::ui {

// Optional app/compiler-supplied paint payload on a scene node (D9). It is intentionally tiny: a
// text run with an optional style override. Visual properties (background, borders, overflow,
// opacity) keep coming from computed style; layout never reads this payload, and changing it marks
// only Paint/Render dirty.
//
// Length rule: at most kMaxTextBytes UTF-8 bytes are stored; longer input is truncated at a UTF-8
// code-point boundary by Node::set_paint_data (documented, no exception).
struct PaintTextStyle {
    // 0 means "use the computed style value"; otherwise the explicit override wins.
    float font_size = 0.0f;
    int font_weight = 0;
    // Empty means "use the computed style color (`color` longhand)".
    std::optional<rendering::rhi::Color> color;

    bool operator==(const PaintTextStyle& other) const {
        if (font_size != other.font_size || font_weight != other.font_weight ||
            color.has_value() != other.color.has_value()) {
            return false;
        }
        if (!color) {
            return true;
        }
        const rendering::rhi::Color& a = *color;
        const rendering::rhi::Color& b = *other.color;
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }
};

enum class PaintKind { Text };

// PaintData text options. Precedence is documented in docs/design/paint-pass.md section 10:
// `wrap` is authoritative for the paint pass (computed `white-space: nowrap|pre` can only suppress
// wrapping, never enable it); nullopt `text_align` falls back to computed `text-align`, then Start;
// `max_lines == 0` means unlimited.
enum class PaintWrap { NoWrap, Wrap };
enum class PaintTextAlign { Start, Center, End };

struct PaintData {
    static constexpr std::size_t kMaxTextBytes = 4096;

    PaintKind kind = PaintKind::Text;
    // UTF-8 text. Empty text paints nothing (the node's background/borders still paint).
    std::string text;
    PaintTextStyle text_style;
    // Wrap opt-in: NoWrap keeps the single-line behavior (V1 default). Wrap uses the content box
    // (border box minus padding) as the wrap width through the shared text-line-box layout.
    PaintWrap wrap = PaintWrap::NoWrap;
    // Maximum emitted lines; 0 means unlimited. Truncation is deterministic (whole computed lines,
    // no ellipsis).
    std::uint32_t max_lines = 0;
    // nullopt uses computed `text-align` (falling back to Start); an explicit value overrides it.
    std::optional<PaintTextAlign> text_align;

    [[nodiscard]] bool empty() const {
        return text.empty();
    }

    bool operator==(const PaintData&) const = default;
};

} // namespace tessera::ui
