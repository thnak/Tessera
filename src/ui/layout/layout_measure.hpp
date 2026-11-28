// SPDX-License-Identifier: MIT
//
// The measure seam of the layout engine: Yoga's measure math, the grid item layouter, the CSS
// `order` permutation and the text-fit fact. Split from layout_engine.cpp, which is at its size
// budget, and shared by the cold tree builder (layout_cold.cpp) and the retained tree
// (layout_retained.cpp) so both paths measure with the very same arithmetic.

#pragma once

#include "grid_engine.hpp"
#include "table_engine.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <yoga/Yoga.h>

namespace tessera::ui::text {
class TextMeasureCache;
}

namespace tessera::ui::layout::detail {

// The config every node in this module is created with: point scale factor 0, so Yoga does not
// round. Positions stay fractional and paint snaps them (pixel_snap.cpp), so a centred mark is
// centred before it is snapped.
[[nodiscard]] YGConfigRef unrounded_config();

// True for `display: grid`/`inline-grid` and for a table (`table_engine.hpp`): the containers whose
// subtree an engine lays out from a measure callback instead of Yoga.
[[nodiscard]] bool is_grid_container(const style::ComputedStyle& style);

// CSS `order` (stylesheet-v1 section 11, issue #213): flex items are laid out in ascending `order`,
// ties keeping source order. Yoga has no `order` style, so the adapter inserts the children in the
// resolved order and walks them back in that same order when collecting boxes. `order` applies to
// every container here because the adapter maps `display: block` to a column flex container, so
// there is no non-flex parent to exempt.
[[nodiscard]] int order_value(const style::ComputedStyle& style);

// Fills `order` with the child indices in layout order and returns true, or returns false (leaving
// `order` empty) when every child is at the initial `0` - the common case, which needs no
// permutation. `keys` is caller-owned scratch, so a frame path reuses its capacity instead of
// allocating a sort key per child per frame.
bool child_layout_order(const LayoutInput& input, std::vector<std::size_t>& order,
                        std::vector<int>& keys);

// The padding paint subtracts from the border box to get its text content box. Paint reads the four
// physical `padding-*` longhands with its own px-only parse (`paint::detail::length_value`): a bare
// number or a `px` value, and 0 for an absent property, a percentage, an em/rem or a negative. The
// text-fit fact is only sound if it uses exactly that box, so this mirrors the parse instead of
// using Yoga's resolved padding (which also resolves percentages and may disagree with paint) or
// `LayoutBox::content_insets` (which counts the border too).
[[nodiscard]] float paint_padding_px(const style::ComputedStyle& style, const char* property);

// Everything the text-fit fact reads, hoisted out of the computed style once per style application
// so box collection never touches the style map. `text` and `style` are borrowed: the holder must
// keep them alive for as long as the inputs are used.
struct TextFitInputs {
    std::string_view text;
    const text::TextStyle* style = nullptr;
    bool text_wrap = false;
    bool display_none = false;
    float pad_left = 0.0f;
    float pad_top = 0.0f;
    float pad_right = 0.0f;
    float pad_bottom = 0.0f;
};

[[nodiscard]] TextFitInputs text_fit_inputs(const LayoutInput& input, const text::TextStyle& style);

// The text-fit fact (issue #490): does this node's text payload provably stay inside the content
// box paint will use? Computed with the very inputs paint derives - the border box minus the
// padding longhands, and paint's wrap decision (`PaintData::wrap` opted in AND the computed
// `white-space` wraps) - and measured through the engine's own cache, so a `Fits` box lets the
// paint-pass cull treat the node like any other bounded box. Only a node that paints text gets a
// fact at all: everything else stays `Unknown`, which paint reads as "stay conservative".
[[nodiscard]] TextFit measure_text_fit(const TextFitInputs& inputs, const rhi::Rect& bounds,
                                       text::TextMeasureCache& cache);
[[nodiscard]] TextFit measure_text_fit(const LayoutInput& input, const rhi::Rect& bounds,
                                       text::TextMeasureCache& cache);

// `display: none` takes the whole subtree out of layout, matching the flex path: every descendant
// reports a zero box at the container's origin.
void emit_zero_boxes(const LayoutInput& input, float x, float y, LayoutResult& result);

// The measure math, shared by the retained tree's callbacks and the cold tree builder.
[[nodiscard]] YGSize measure_text_box(std::string_view text, const text::TextStyle& style,
                                      text::TextMeasureCache& cache, float width,
                                      YGMeasureMode width_mode, float height,
                                      YGMeasureMode height_mode);
[[nodiscard]] YGSize measure_grid_box(const LayoutInput& container, const LayoutViewport& viewport,
                                      text::TextMeasureCache& cache, float width,
                                      YGMeasureMode width_mode, float height,
                                      YGMeasureMode height_mode);

// What a grid item's layouter reads. Lives on the stack of whoever runs the grid, so it outlives
// the `LayoutInput` the grid engine was handed.
struct GridItemContext {
    const LayoutViewport* viewport = nullptr;
    text::TextMeasureCache* cache = nullptr;
};

// Lays out one grid item subtree for `GridEngine`. The item's border box is the subtree root, so
// the returned size is the size the engine positions and aligns.
[[nodiscard]] GridItemLayout layout_grid_item(void* raw, const LayoutInput& item,
                                              float available_width, float available_height,
                                              bool fill, std::vector<LayoutBox>* boxes);

void run_grid(const LayoutInput& container, const LayoutViewport& viewport,
              const GridContentBox& content, text::TextMeasureCache& cache, float origin_x,
              float origin_y, std::vector<LayoutBox>* boxes, float& out_width, float& out_height);

} // namespace tessera::ui::layout::detail
