// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <algorithm>
#include <array>
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

enum class PaintKind { Text, Vector, Image };

// PaintData image options (embedded PNG/JPEG asset). Mirrors PaintVectorStyle: the payload carries
// the borrowed asset name, optional device-pixel raster size, scale hint, tint and opacity, and the
// active PaintKind selects it (text/vector/image are mutually exclusive per node).
struct PaintImageStyle {
    std::string_view asset;
    std::uint32_t raster_width = 0;
    std::uint32_t raster_height = 0;
    float scale = 0.0f;
    std::optional<rendering::rhi::Color> tint;
    float opacity = 1.0f;
    // `Image decodeSize="original"`: decode at source size instead of display size x DPR.
    bool decode_original = false;

    bool operator==(const PaintImageStyle& other) const {
        if (asset != other.asset || raster_width != other.raster_width ||
            raster_height != other.raster_height || scale != other.scale ||
            opacity != other.opacity || decode_original != other.decode_original) {
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

// Caret/selection decoration for a paint text run. Paint consumes only byte offsets: it resolves
// them to line boxes through the same metrics source/layout the wrap uses, so the paint pass needs
// no edit-buffer knowledge. `selection_begin/end` are a half-open UTF-8 byte range (end may be less
// than begin in the payload; paint normalizes). Newline bytes map to the end of the preceding line;
// a caret at a line break is drawn at the line end (before the break), and a caret at the text end
// is drawn after the last glyph. `blink_phase < 0.5` hides the caret; paint has no timers.
struct TextDecoration {
    std::size_t caret_byte_offset = 0;
    std::size_t selection_begin = 0;
    std::size_t selection_end = 0;
    bool caret_visible = true;
    float blink_phase = 1.0f;
    // IME preedit (paint-pass.md section 10.2, text-editing-v1 section 12). While composing, the
    // preedit bytes are NOT part of `PaintData::text`: `preedit_begin`/`preedit_end` are the
    // half-open buffer bytes the composition will replace (equal offsets = insertion point) and
    // `preedit_text` holds the uncommitted UTF-8 bytes to render at that point. Paint has no timers
    // and never reads the edit controller; `composition_active` gates the descriptor and
    // `preedit_underline` draws the underline under the preedit run (or under the replaced range
    // when `preedit_text` is empty, i.e. the composition is already in the painted text).
    bool composition_active = false;
    std::size_t preedit_begin = 0;
    std::size_t preedit_end = 0;
    bool preedit_underline = true;
    std::string preedit_text;

    [[nodiscard]] bool has_selection() const {
        return selection_begin != selection_end;
    }
    bool operator==(const TextDecoration&) const = default;
};

// Linear gradient background (V1). Stops are clamped to [0,1] and sorted by offset; equal offsets
// keep their input order and the later stop wins at the duplicate sample point (documented rule).
// `angle_degrees` uses the CSS convention: 0 = to top, 90 = to right, 180 = to bottom.
// Interpolation is encoded-space (sRGB bytes, no linearization) — the V1 sRGB approximation — and
// the paint pass folds node opacity into each stop's alpha before handing the gradient to a sink.
struct LinearGradientStop {
    float offset = 0.0f;
    rendering::rhi::Color color;

    bool operator==(const LinearGradientStop& other) const {
        return offset == other.offset && color.r == other.color.r && color.g == other.color.g &&
               color.b == other.color.b && color.a == other.color.a;
    }
};

struct LinearGradient {
    static constexpr std::size_t kMaxStops = 8;
    float angle_degrees = 180.0f;
    std::array<LinearGradientStop, kMaxStops> stops{};
    std::size_t stop_count = 0;

    bool operator==(const LinearGradient& other) const {
        return angle_degrees == other.angle_degrees && stop_count == other.stop_count &&
               std::equal(stops.begin(), stops.begin() + stop_count, other.stops.begin());
    }
};

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
    // Optional caret/selection decoration (paint-only; the caller derives offsets from the edit
    // model). Absent keeps the previous output byte-identical.
    std::optional<TextDecoration> decoration;
    // Vector payload; only painted when kind == PaintKind::Vector.
    PaintVectorStyle vector;
    // Image payload; only painted when kind == PaintKind::Image.
    PaintImageStyle image;
    // Optional linear-gradient background, painted where the background would be (node box,
    // rounded by border-radius) under content/text/vector/image draws. Absent keeps the solid
    // background-color behavior.
    std::optional<LinearGradient> gradient;

    [[nodiscard]] bool empty() const {
        if (kind == PaintKind::Vector) {
            return vector.asset.empty() || vector.asset.size() > kMaxAssetBytes;
        }
        if (kind == PaintKind::Image) {
            return image.asset.empty() || image.asset.size() > kMaxAssetBytes;
        }
        return text.empty();
    }

    bool operator==(const PaintData&) const = default;
};

} // namespace tessera::ui
