// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

enum class PaintKind { Text, Vector };

// PaintData vector options (embedded SVG asset). The asset name is a BORROWED view, consistent with
// the node metadata rule (generated code passes string literals, whose storage is static); the
// paint pass hands it straight to the sink for the duration of the paint call.
//
// Precedence: `PaintKind` selects the payload that is painted; fields of the other kind are ignored
// (a node is either text or a vector asset, never both). `empty()` follows the active kind.
//
// Raster size:
// - raster_width/height are device pixels; 0 in both means "derive from the content box and the
//   sink's device scale";
// - scale > 0 is an explicit uniform scale multiplier applied to the sink device scale (0 = use the
//   sink default). Explicit raster sizes take precedence over scale-derived sizes.
// Tint multiplies the sampled colors (white opaque by default); its alpha is further multiplied by
// the node opacity and `opacity` (clamped 0..1).
struct PaintVectorStyle {
    std::string_view asset;
    std::uint32_t raster_width = 0;
    std::uint32_t raster_height = 0;
    float scale = 0.0f;
    std::optional<rendering::rhi::Color> tint;
    float opacity = 1.0f;

    bool operator==(const PaintVectorStyle& other) const {
        if (asset != other.asset || raster_width != other.raster_width ||
            raster_height != other.raster_height || scale != other.scale ||
            opacity != other.opacity) {
            return false;
        }
        if (tint.has_value() != other.tint.has_value()) {
            return false;
        }
        if (!tint) {
            return true;
        }
        const rendering::rhi::Color& a = *tint;
        const rendering::rhi::Color& b = *other.tint;
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }
};

// PaintData text options. Precedence is documented in docs/design/paint-pass.md section 10:
// `wrap` is authoritative for the paint pass (computed `white-space: nowrap|pre` can only suppress
// wrapping, never enable it); nullopt `text_align` falls back to computed `text-align`, then Start;
// `max_lines == 0` means unlimited.
enum class PaintWrap { NoWrap, Wrap };
enum class PaintTextAlign { Start, Center, End };

struct PaintData {
    static constexpr std::size_t kMaxTextBytes = 4096;
    // Embedded asset names are short by construction; longer names are ignored by the paint pass
    // (never truncated, because truncation would silently paint the wrong asset).
    static constexpr std::size_t kMaxAssetBytes = 256;

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
    // Vector payload; only painted when kind == PaintKind::Vector (text fields are then ignored).
    PaintVectorStyle vector;

    [[nodiscard]] bool empty() const {
        if (kind == PaintKind::Vector) {
            return vector.asset.empty() || vector.asset.size() > kMaxAssetBytes;
        }
        return text.empty();
    }

    bool operator==(const PaintData&) const = default;
};

} // namespace tessera::ui
