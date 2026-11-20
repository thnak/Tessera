// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The `background-image` layer list (issue #614).
//
// CSS Backgrounds 3 section 2.1 makes `background-image` a comma-separated list of images, painted
// bottom to top: the **last** layer is painted first and the **first** layer ends up on top. V1
// implements the list of `linear-gradient()` layers plus `none`; a `url()` layer is not in scope
// (images are not a `background-image` value the engine paints yet), so it is rejected with a
// diagnostic rather than silently dropped.
//
// The list is deliberately a list of *layers*, not of gradients: `BackgroundLayer` carries a kind
// tag, so a radial/conic layer (#613, #617) or a `url()` layer drops in as a new enumerator and a
// new field without changing the parse, the paint order or the interpolation contract.
//
// The `background` shorthand's colour is **not** a layer here: CSS puts it on the final layer only
// and it lands in `background-color`, which paints underneath every layer. A colour on any other
// layer is rejected, as in CSS.
namespace tessera::ui::style {

// The number of layers a `background-image` value may carry. CSS has no limit; this bounds the
// fixed-size value so parsing stays allocation-free, and 8 is far past what a theme stacks.
inline constexpr std::size_t kMaxBackgroundLayers = 8;

enum class BackgroundLayerKind : unsigned char {
    // `none`: a transparent layer. It paints nothing, but it keeps its slot so a list can be padded
    // during interpolation (a `none` layer fades a gradient in or out).
    None,
    LinearGradient,
};

struct BackgroundLayer {
    BackgroundLayerKind kind = BackgroundLayerKind::None;
    // Meaningful only for `LinearGradient`.
    LinearGradientValue gradient;
};

// CSS order: `layers[0]` is the topmost layer and `layers[count - 1]` is painted first.
struct BackgroundLayerList {
    std::array<BackgroundLayer, kMaxBackgroundLayers> layers{};
    std::size_t count = 0;

    [[nodiscard]] bool empty() const {
        return count == 0;
    }
};

// Parses a `background-image` value: a comma-separated list of `linear-gradient()` and `none`.
// Returns nullopt when any layer is outside that grammar (a `url()`, a radial/conic gradient, a
// `repeating-*`, more than `kMaxBackgroundLayers` layers, or an empty list), so the caller reports
// the value instead of painting a guess. `none` alone is a valid, empty list.
[[nodiscard]] std::optional<BackgroundLayerList> parse_background_layers(std::string_view text);

// The value text of a list, in CSS order, so a parsed list round-trips through the parser. Used by
// the interpolator and by diagnostics.
[[nodiscard]] std::string format_background_layers(const BackgroundLayerList& list);

// Interpolates two `background-image` values at progress `t` (CSS Images 4 section 3.1,
// "interpolate the image list"). The shorter list is padded with `none` layers at the **bottom**,
// so a layer added on top fades in instead of snapping; a `none` paired with a gradient becomes
// that gradient with every stop's alpha at 0, which is the transparent image `none` denotes. Two
// gradients interpolate their angle along the shorter arc and their stops pairwise, padding the
// shorter stop list by repeating its last stop, as CSS does. Returns nullopt when either value is
// not a layer list, so the caller keeps its discrete switch.
[[nodiscard]] std::optional<std::string>
interpolate_background_layers(std::string_view from, std::string_view to, double t);

} // namespace tessera::ui::style
