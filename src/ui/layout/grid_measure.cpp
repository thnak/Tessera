// SPDX-License-Identifier: MIT
//
// Item measurement for the CSS Grid subset (docs/specs/grid-v1.md section 4): the min- and
// max-content contributions the track sizing reads, and the re-measure of an item's height at the
// width its column resolved to.

#include "grid_engine.hpp"
#include "layout_style.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace tessera::ui::layout::detail {
namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

} // namespace

float item_min_content_width(const GridItemLayouter& layouter, const LayoutInput& item,
                             const GridItemLayout& maximum, bool definite_width) {
    // A text leaf measured at zero available width reports its longest unbreakable run, which is
    // the min-content width. A container at zero width reports just its insets (its text
    // overflows), so its min-content is how far its laid-out descendants reach, capped by the
    // max-content width (a fixed width, or size containment, stays what it is).
    if (!item.text.empty() && item.children.empty()) {
        return layouter.layout(item, 0.0f, -1.0f, nullptr).width;
    }
    if (item.children.empty() || definite_width) {
        return maximum.width;
    }
    std::vector<LayoutBox> narrow;
    (void)layouter.layout(item, 0.0f, -1.0f, &narrow);
    float reach = 0.0f;
    float first_x = kInfinity; // the left inset: where the first descendants start
    for (std::size_t i = 1; i < narrow.size(); ++i) {
        reach = std::max(reach, narrow[i].bounds.x + narrow[i].bounds.width);
        first_x = std::min(first_x, narrow[i].bounds.x);
    }
    // At zero width the box is its insets, so the right inset is what the left one leaves.
    const float right_inset = narrow.empty() || first_x == kInfinity
                                  ? 0.0f
                                  : std::max(0.0f, narrow.front().bounds.width - first_x);
    return std::min(reach + right_inset, maximum.width);
}

void GridEngine::measure_items() {
    bool need_width = columns_.needs_content;
    bool need_height = rows_.needs_content;
    for (const Item& item : items_) {
        if (item.justify_self != GridAlign::Stretch) {
            need_width = true;
        }
        if (item.align_self != GridAlign::Stretch) {
            need_height = true;
        }
        if (item.column_start + item.column_span - 1 >
            static_cast<int>(columns_.explicit_tracks.size())) {
            need_width = true;
        }
        if (item.row_start + item.row_span - 1 > static_cast<int>(rows_.explicit_tracks.size())) {
            need_height = true;
        }
    }
    heights_measured_ = need_height;
    for (Item& item : items_) {
        if (!need_width && !need_height) {
            continue;
        }
        const GridItemLayout maximum = layouter_.layout(*item.input, -1.0f, -1.0f, nullptr);
        item.max_content_width = maximum.width;
        item.max_content_height = maximum.height;
        // A text leaf measured at zero available width reports its longest unbreakable run, which
        // is the min-content width. A container at zero width reports just its insets (its text
        // overflows), so its min-content is how far its laid-out descendants reach, capped by the
        // max-content width (a fixed width, or size containment, stays what it is).
        item.min_content_width =
            need_width ? item_min_content_width(layouter_, *item.input, maximum,
                                                has_definite_size(*item.input, "width"))
                       : maximum.width;
    }
}

// An item's height depends on the width it is given: text wraps at its column. The first pass
// measured at max-content width, which would size an auto row for the unwrapped text, so once the
// columns are known the stretched items are measured again at their column width.
void GridEngine::remeasure_heights_at_column_widths() {
    if (!heights_measured_) {
        return;
    }
    for (Item& item : items_) {
        if (item.justify_self != GridAlign::Stretch || !item.width_auto || !item.height_auto) {
            continue;
        }
        float cell_width = columns_.step * static_cast<float>(item.column_span - 1);
        for (int i = 0; i < item.column_span; ++i) {
            cell_width += columns_.base[static_cast<std::size_t>(item.column_start - 1 + i)];
        }
        const float inner = std::max(0.0f, cell_width - item.margin_left - item.margin_right);
        item.max_content_height = layouter_.layout(*item.input, inner, -1.0f, nullptr).height;
    }
}

} // namespace tessera::ui::layout::detail
