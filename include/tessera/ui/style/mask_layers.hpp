// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/background_layers.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

// The per-layer geometry of a `mask-image` layer list (issue #617): `mask-size`, `mask-position`,
// `mask-repeat`, `mask-mode`, `mask-origin`, `mask-clip` and `mask-composite`. Each is a comma-separated list that
// lines up with the layers of `mask-image`; a list shorter than the layer list repeats cyclically
// (CSS Masking 1 section 6.1), and an absent list means the initial value on every layer.
//
// V1 takes lengths in px and percentages only (no em/rem/calc), because the paint pass has no
// font-size at the mask. Gradients have no intrinsic size, so `auto`, `cover` and `contain` all
// resolve to the mask positioning area (the border box) per CSS Images 3 section 5.
namespace tessera::ui::style {

enum class MaskMode : unsigned char {
    // `match-source`: a gradient is an image, so it behaves as `alpha`.
    MatchSource,
    Alpha,
    // The mask value is the luminance of the image times its alpha (sRGB coefficients).
    Luminance,
};

// A box of the node: the positioning area (`mask-origin`) or the painting area (`mask-clip`).
// `NoClip` is only a `mask-clip` value: the layer is not cut to any box. `fill-box` is the content
// box and `stroke-box` and `view-box` are the border box for an element with a CSS layout box (CSS
// Masking 1 section 6.2); `margin-box` is refused because the paint pass has no margins.
enum class MaskBox : unsigned char { Border, Padding, Content, NoClip };

// How a layer combines with the layers below it (CSS Masking 1 section 6.9). `Add` is source-over,
// `Subtract` keeps the layer where the layers below are empty (source-out), `Intersect` keeps it
// where they are not (source-in) and `Exclude` is xor. The bottom layer has nothing below it, and its
// value is ignored.
enum class MaskComposite : unsigned char { Add, Subtract, Intersect, Exclude };

enum class MaskRepeatKind : unsigned char { Repeat, Space, Round, NoRepeat };

// One axis of `mask-size`: `auto`, a px length or a percentage of the positioning area.
struct MaskLength {
    enum class Kind : unsigned char { Auto, Px, Percent };
    Kind kind = Kind::Auto;
    float value = 0.0f;
};

struct MaskSize {
    bool cover = false;
    bool contain = false;
    MaskLength width;
    MaskLength height;
};

// An offset along one axis: `(area - tile) * fraction + px`. `right 10px` is fraction 1, px -10.
struct MaskPositionAxis {
    float fraction = 0.0f;
    float px = 0.0f;
};

struct MaskPosition {
    MaskPositionAxis x;
    MaskPositionAxis y;
};

struct MaskRepeat {
    MaskRepeatKind x = MaskRepeatKind::Repeat;
    MaskRepeatKind y = MaskRepeatKind::Repeat;
};

// A fixed-size list, so parsing stays allocation-free like the layer list.
template<typename T> struct MaskValueList {
    std::array<T, kMaxBackgroundLayers> values{};
    std::size_t count = 0;

    // CSS repeats a short list cyclically to match the image list; an empty list is the initial.
    [[nodiscard]] T at_layer(std::size_t layer) const {
        return count == 0 ? T{} : values[layer % count];
    }
};

// One layer's value. Each returns nullopt outside the grammar.
//   size:     auto | cover | contain | <len-pct> [<len-pct> | auto]
//   position: 1 to 4 values: keywords (left/right/top/bottom/center), lengths and percentages,
//             including the `right 10px bottom 20px` edge-offset form
//   repeat:   repeat-x | repeat-y | [repeat | space | round | no-repeat]{1,2}
//   mode:     alpha | luminance | match-source
//   origin:   border-box | padding-box | content-box (and fill-box, stroke-box, view-box)
//   clip:     the origin values or no-clip
//   composite: add | subtract | intersect | exclude
[[nodiscard]] std::optional<MaskSize> parse_mask_size(std::string_view text);
[[nodiscard]] std::optional<MaskPosition> parse_mask_position(std::string_view text);
[[nodiscard]] std::optional<MaskRepeat> parse_mask_repeat(std::string_view text);
[[nodiscard]] std::optional<MaskMode> parse_mask_mode(std::string_view text);

[[nodiscard]] std::optional<MaskComposite> parse_mask_composite(std::string_view text);
[[nodiscard]] std::optional<MaskBox> parse_mask_origin(std::string_view text);
[[nodiscard]] std::optional<MaskBox> parse_mask_clip(std::string_view text);

// The comma-separated lists. nullopt when any entry is outside the grammar, the list is empty, or
// it has more than `kMaxBackgroundLayers` entries.
[[nodiscard]] std::optional<MaskValueList<MaskSize>> parse_mask_size_list(std::string_view text);
[[nodiscard]] std::optional<MaskValueList<MaskPosition>>
parse_mask_position_list(std::string_view text);
[[nodiscard]] std::optional<MaskValueList<MaskRepeat>>
parse_mask_repeat_list(std::string_view text);
[[nodiscard]] std::optional<MaskValueList<MaskMode>> parse_mask_mode_list(std::string_view text);
[[nodiscard]] std::optional<MaskValueList<MaskComposite>>
parse_mask_composite_list(std::string_view text);
[[nodiscard]] std::optional<MaskValueList<MaskBox>> parse_mask_origin_list(std::string_view text);
[[nodiscard]] std::optional<MaskValueList<MaskBox>> parse_mask_clip_list(std::string_view text);

} // namespace tessera::ui::style
