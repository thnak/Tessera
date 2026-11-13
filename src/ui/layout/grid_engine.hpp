// SPDX-License-Identifier: MIT
//
// The CSS Grid subset engine (issue #249, docs/specs/grid-v1.md). Internal to src/ui/layout: UI
// code never includes this header, and `LayoutEngine` is its only caller. The engine owns track
// sizing, placement and alignment; it delegates item layout back to the caller through
// `GridItemLayouter`, so a grid item subtree is still laid out by the flex engine.

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/style/grid_values.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::ui::layout::detail {

// How a grid item is aligned inside its grid area. `Start`/`End` are logical (they mirror with
// `direction: rtl`); `Left`/`Right` are physical.
enum class GridAlign : std::uint8_t {
    Start,
    End,
    Center,
    Stretch,
    Left,
    Right,
};

// How the track grid is aligned inside the container when the tracks do not fill it.
enum class GridContentAlign : std::uint8_t {
    Start,
    End,
    Center,
    Stretch,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

// One item's border-box size, as measured by the layouter.
struct GridItemLayout {
    float width = 0.0f;
    float height = 0.0f;
};

// Lays out one item subtree in isolation with the item's border box at (0, 0). A negative
// `available_width`/`available_height` means unconstrained. The callback must not apply the item's
// own margins: the grid engine positions the border box and accounts for margins itself. When
// `boxes` is non-null the item's boxes are appended to it.
using GridItemLayoutFn = GridItemLayout (*)(void* context, const LayoutInput& item,
                                            float available_width, float available_height,
                                            std::vector<LayoutBox>* boxes);

struct GridItemLayouter {
    GridItemLayoutFn fn = nullptr;
    void* context = nullptr;

    [[nodiscard]] GridItemLayout layout(const LayoutInput& item, float available_width,
                                        float available_height,
                                        std::vector<LayoutBox>* boxes) const {
        return fn(context, item, available_width, available_height, boxes);
    }
};

// The container's content box. A dimension that is not definite is sized to the grid's content.
struct GridContentBox {
    float width = 0.0f;
    float height = 0.0f;
    bool width_definite = true;
    bool height_definite = true;
};

class GridEngine {
public:
    GridEngine(const LayoutInput& container, const LayoutViewport& viewport,
               const GridItemLayouter& layouter);

    // Runs track parsing, placement, track sizing and item positioning for `content`.
    void run(const GridContentBox& content);

    // The content-box size the grid needs. Used by the container's measure callback.
    [[nodiscard]] float content_width() const {
        return content_width_;
    }
    [[nodiscard]] float content_height() const {
        return content_height_;
    }

    // Appends the positioned item boxes, with the content-box origin at (origin_x, origin_y).
    void emit_boxes(float origin_x, float origin_y, std::vector<LayoutBox>& boxes) const;

private:
    struct Item {
        const LayoutInput* input = nullptr;
        style::GridLine column;
        style::GridLine row;
        GridAlign justify_self = GridAlign::Stretch;
        GridAlign align_self = GridAlign::Stretch;
        int column_start = 1;
        int column_span = 1;
        int row_start = 1;
        int row_span = 1;
        bool column_auto = true;
        bool row_auto = true;
        bool width_auto = true;
        bool height_auto = true;
        float margin_left = 0.0f;
        float margin_top = 0.0f;
        float margin_right = 0.0f;
        float margin_bottom = 0.0f;
        float min_content_width = 0.0f;
        float max_content_width = 0.0f;
        float max_content_height = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        std::size_t box_begin = 0;
        std::size_t box_end = 0;
    };

    struct Axis {
        std::vector<style::GridTrack> explicit_tracks;
        std::vector<float> base;
        std::vector<float> growth_limit;
        std::vector<float> fr;
        std::vector<bool> content_based;
        std::vector<bool> auto_max;
        std::vector<float> offsets;
        float gap = 0.0f;
        float step = 0.0f;
        float total = 0.0f;
        int count = 0;
        bool needs_content = false;
    };

    void parse_tracks();
    void place_items();
    void measure_items();
    void size_axis(Axis& axis, float container_size, bool definite, bool is_column,
                   GridContentAlign content_align);
    void position_items(float mirror_width);
    [[nodiscard]] float resolve_length(const style::GridTrackSize& size) const;
    [[nodiscard]] float resolve_breadth(const style::GridTrackSize& size, float container_size,
                                        bool definite, bool& content_based, bool& flexible) const;
    [[nodiscard]] float resolve_gap(const char* name, float percent_base) const;
    [[nodiscard]] bool has_definite_size(const LayoutInput& item, const char* property) const;
    [[nodiscard]] float contribution(const Item& item, bool is_column, bool maximum) const;

    const LayoutInput& container_;
    const LayoutViewport& viewport_;
    GridItemLayouter layouter_;
    std::vector<Item> items_;
    Axis columns_;
    Axis rows_;
    std::vector<LayoutBox> boxes_;
    bool rtl_ = false;
    bool column_flow_ = false;
    GridAlign justify_items_ = GridAlign::Stretch;
    GridAlign align_items_ = GridAlign::Stretch;
    GridContentAlign justify_content_ = GridContentAlign::Start;
    GridContentAlign align_content_ = GridContentAlign::Start;
    float content_width_ = 0.0f;
    float content_height_ = 0.0f;
};

} // namespace tessera::ui::layout::detail
