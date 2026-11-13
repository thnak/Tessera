// SPDX-License-Identifier: MIT
//
// Track sizing and item measurement for the CSS Grid subset (docs/specs/grid-v1.md section 4).
//
// The algorithm is a documented subset of CSS Grid section 12: intrinsic track sizes come from the
// items' max-content contributions, then free space is distributed to `fr` tracks, then `auto`
// tracks stretch. There is no min-content pass beyond measuring a text leaf at zero width, so a
// content-sized track never shrinks below its item's unwrapped size.

#include "grid_engine.hpp"
#include "layout_style.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace tessera::ui::layout::detail {
namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

style::GridTrack implicit_track() {
    const style::GridTrackSize auto_size{style::GridTrackKind::Auto, 0.0f,
                                         style::GridLengthUnit::None};
    return style::GridTrack{auto_size, auto_size};
}

// Grows the spanned tracks toward `target`, sharing the deficit equally among the tracks that are
// still below `limits`. Finite limits are honoured, so a `minmax(<length>, <length>)` track cannot
// exceed its maximum.
void grow_tracks(std::vector<float>& sizes, const std::vector<bool>& growable,
                 const std::vector<float>& limits, int start, int span, float target) {
    float current = 0.0f;
    for (int i = 0; i < span; ++i) {
        current += sizes[static_cast<std::size_t>(start + i)];
    }
    float deficit = target - current;
    if (deficit <= 0.0f) {
        return;
    }
    for (int pass = 0; pass < span && deficit > 0.0001f; ++pass) {
        int candidates = 0;
        for (int i = 0; i < span; ++i) {
            const auto index = static_cast<std::size_t>(start + i);
            if (growable[index] && sizes[index] < limits[index]) {
                ++candidates;
            }
        }
        if (candidates == 0) {
            return;
        }
        const float share = deficit / static_cast<float>(candidates);
        float grown = 0.0f;
        for (int i = 0; i < span; ++i) {
            const auto index = static_cast<std::size_t>(start + i);
            if (!growable[index] || sizes[index] >= limits[index]) {
                continue;
            }
            const float delta = std::min(share, limits[index] - sizes[index]);
            sizes[index] += delta;
            grown += delta;
        }
        if (grown <= 0.0f) {
            return;
        }
        deficit -= grown;
    }
}

} // namespace

float GridEngine::resolve_length(const style::GridTrackSize& size) const {
    switch (size.unit) {
    case style::GridLengthUnit::Px:
        return size.value;
    case style::GridLengthUnit::Rem:
    case style::GridLengthUnit::Em:
        return size.value * 16.0f;
    case style::GridLengthUnit::Vw:
        return size.value * viewport_.width / 100.0f;
    case style::GridLengthUnit::Vh:
        return size.value * viewport_.height / 100.0f;
    case style::GridLengthUnit::Pt:
        return size.value * 4.0f / 3.0f;
    case style::GridLengthUnit::None:
        return size.value;
    }
    return size.value;
}

float GridEngine::resolve_breadth(const style::GridTrackSize& size, float container_size,
                                  bool definite, bool& content_based, bool& flexible) const {
    content_based = false;
    flexible = false;
    switch (size.kind) {
    case style::GridTrackKind::Fixed:
        return resolve_length(size);
    case style::GridTrackKind::Percent:
        if (!definite) {
            // A percentage track in an indefinite container behaves as `auto`.
            content_based = true;
            return 0.0f;
        }
        return container_size * size.value / 100.0f;
    case style::GridTrackKind::Fr:
        flexible = true;
        return 0.0f;
    case style::GridTrackKind::Auto:
    case style::GridTrackKind::MinContent:
    case style::GridTrackKind::MaxContent:
        content_based = true;
        return 0.0f;
    }
    return 0.0f;
}

float GridEngine::resolve_gap(const char* name, float percent_base) const {
    const std::string* value = container_.style.find(name);
    if (value == nullptr) {
        return 0.0f;
    }
    style::GridTrackSize size;
    if (!style::parse_grid_length(*value, size)) {
        // `normal` (the initial value) and anything unsupported mean no gap.
        return 0.0f;
    }
    if (size.kind == style::GridTrackKind::Fixed) {
        return resolve_length(size);
    }
    if (size.kind == style::GridTrackKind::Percent && percent_base > 0.0f) {
        return percent_base * size.value / 100.0f;
    }
    return 0.0f;
}

bool GridEngine::has_definite_size(const LayoutInput& item, const char* property) const {
    const std::string* value = item.style.find(property);
    if (value == nullptr) {
        return false;
    }
    style::GridTrackSize size;
    if (!style::parse_grid_length(*value, size)) {
        return false;
    }
    return size.kind == style::GridTrackKind::Fixed || size.kind == style::GridTrackKind::Percent;
}

float GridEngine::contribution(const Item& item, bool is_column, bool maximum) const {
    if (is_column) {
        const float size = maximum ? item.max_content_width : item.min_content_width;
        return size + item.margin_left + item.margin_right;
    }
    return item.max_content_height + item.margin_top + item.margin_bottom;
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
    for (Item& item : items_) {
        if (!need_width && !need_height) {
            continue;
        }
        const GridItemLayout maximum = layouter_.layout(*item.input, -1.0f, -1.0f, nullptr);
        item.max_content_width = maximum.width;
        item.max_content_height = maximum.height;
        // A text leaf measured at zero available width reports its longest unbreakable run, which
        // is the min-content width. A container would report zero there, so it reuses max-content.
        if (need_width && !item.input->text.empty() && item.input->children.empty()) {
            item.min_content_width = layouter_.layout(*item.input, 0.0f, -1.0f, nullptr).width;
        } else {
            item.min_content_width = maximum.width;
        }
    }
}

void GridEngine::size_axis(Axis& axis, float container_size, bool definite, bool is_column,
                           GridContentAlign content_align) {
    const int count = std::max(axis.count, 1);
    const auto size = static_cast<std::size_t>(count);
    axis.base.assign(size, 0.0f);
    axis.growth_limit.assign(size, kInfinity);
    axis.fr.assign(size, 0.0f);
    axis.content_based.assign(size, false);
    axis.auto_max.assign(size, false);
    axis.offsets.assign(size, 0.0f);
    axis.needs_content = false;

    for (int i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const style::GridTrack track =
            index < axis.explicit_tracks.size() ? axis.explicit_tracks[index] : implicit_track();
        bool min_content = false;
        bool min_flex = false;
        const float base =
            resolve_breadth(track.min, container_size, definite, min_content, min_flex);
        bool max_content = false;
        bool max_flex = false;
        const float limit =
            resolve_breadth(track.max, container_size, definite, max_content, max_flex);
        axis.base[index] = base;
        if (max_flex) {
            axis.fr[index] = track.max.value;
            axis.growth_limit[index] = kInfinity;
        } else if (max_content) {
            axis.growth_limit[index] = base;
        } else {
            axis.growth_limit[index] = std::max(limit, base);
        }
        axis.content_based[index] = min_content || max_content;
        axis.auto_max[index] = track.max.kind == style::GridTrackKind::Auto;
        axis.needs_content = axis.needs_content || axis.content_based[index];
    }

    // Intrinsic sizing: every item grows the content-based tracks it spans.
    const std::vector<float> unlimited(size, kInfinity);
    for (const Item& item : items_) {
        const int start = (is_column ? item.column_start : item.row_start) - 1;
        const int span = is_column ? item.column_span : item.row_span;
        if (start < 0 || start + span > count) {
            continue;
        }
        grow_tracks(axis.base, axis.content_based, unlimited, start, span,
                    contribution(item, is_column, false));
        grow_tracks(axis.growth_limit, axis.content_based, unlimited, start, span,
                    contribution(item, is_column, true));
    }
    for (int i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        axis.growth_limit[index] = std::max(axis.growth_limit[index], axis.base[index]);
    }

    // Maximize tracks: distribute free space to the non-flexible tracks up to their growth limits.
    float used = axis.gap * static_cast<float>(count - 1);
    for (const float value : axis.base) {
        used += value;
    }
    if (definite && container_size > used) {
        float remaining = container_size - used;
        for (int guard = 0; guard < count && remaining > 0.001f; ++guard) {
            int growable = 0;
            for (int i = 0; i < count; ++i) {
                const auto index = static_cast<std::size_t>(i);
                if (axis.fr[index] == 0.0f && axis.base[index] < axis.growth_limit[index]) {
                    ++growable;
                }
            }
            if (growable == 0) {
                break;
            }
            const float share = remaining / static_cast<float>(growable);
            float grown = 0.0f;
            for (int i = 0; i < count; ++i) {
                const auto index = static_cast<std::size_t>(i);
                if (axis.fr[index] != 0.0f || axis.base[index] >= axis.growth_limit[index]) {
                    continue;
                }
                const float delta = std::min(share, axis.growth_limit[index] - axis.base[index]);
                axis.base[index] += delta;
                grown += delta;
            }
            if (grown <= 0.0f) {
                break;
            }
            remaining -= grown;
        }
    }

    // Expand flexible tracks. A track whose base size already exceeds its hypothetical `fr` share
    // keeps the base size and stops being flexible, so `minmax(100px, 1fr)` never shrinks below
    // its minimum and the remaining `fr` tracks take the space it releases.
    bool any_flex = false;
    for (const float value : axis.fr) {
        any_flex = any_flex || value > 0.0f;
    }
    if (definite && any_flex) {
        std::vector<bool> flexible(size, false);
        for (int i = 0; i < count; ++i) {
            flexible[static_cast<std::size_t>(i)] = axis.fr[static_cast<std::size_t>(i)] > 0.0f;
        }
        const float available_for_tracks =
            container_size - axis.gap * static_cast<float>(count - 1);
        for (int guard = 0; guard < count; ++guard) {
            float flex_sum = 0.0f;
            float fixed_size = 0.0f;
            for (int i = 0; i < count; ++i) {
                const auto index = static_cast<std::size_t>(i);
                if (flexible[index]) {
                    flex_sum += axis.fr[index];
                } else {
                    fixed_size += axis.base[index];
                }
            }
            if (flex_sum <= 0.0f) {
                break;
            }
            const float fr_size = (available_for_tracks - fixed_size) / flex_sum;
            bool clamped = false;
            for (int i = 0; i < count; ++i) {
                const auto index = static_cast<std::size_t>(i);
                if (flexible[index] && axis.base[index] > fr_size * axis.fr[index]) {
                    flexible[index] = false;
                    clamped = true;
                }
            }
            if (!clamped) {
                for (int i = 0; i < count; ++i) {
                    const auto index = static_cast<std::size_t>(i);
                    if (flexible[index]) {
                        axis.base[index] = std::max(axis.base[index], fr_size * axis.fr[index]);
                    }
                }
                break;
            }
        }
    }

    // Stretch `auto` tracks when the grid's content alignment is `normal`/`stretch` (the CSS
    // default for a grid container).
    if (definite && content_align == GridContentAlign::Stretch) {
        used = axis.gap * static_cast<float>(count - 1);
        for (const float value : axis.base) {
            used += value;
        }
        if (container_size > used) {
            int stretchable = 0;
            for (int i = 0; i < count; ++i) {
                const auto index = static_cast<std::size_t>(i);
                if (axis.auto_max[index] && axis.fr[index] == 0.0f) {
                    ++stretchable;
                }
            }
            if (stretchable > 0) {
                const float share = (container_size - used) / static_cast<float>(stretchable);
                for (int i = 0; i < count; ++i) {
                    const auto index = static_cast<std::size_t>(i);
                    if (axis.auto_max[index] && axis.fr[index] == 0.0f) {
                        axis.base[index] += share;
                    }
                }
            }
        }
    }

    float total = axis.gap * static_cast<float>(count - 1);
    for (const float value : axis.base) {
        total += value;
    }
    axis.total = total;

    // Grid content alignment: place the track grid inside the container.
    const float available = definite ? container_size : total;
    const float leftover = std::max(0.0f, available - total);
    float offset = 0.0f;
    float extra_gap = 0.0f;
    switch (content_align) {
    case GridContentAlign::Start:
    case GridContentAlign::Stretch:
        break;
    case GridContentAlign::End:
        offset = leftover;
        break;
    case GridContentAlign::Center:
        offset = leftover / 2.0f;
        break;
    case GridContentAlign::SpaceBetween:
        extra_gap = count > 1 ? leftover / static_cast<float>(count - 1) : 0.0f;
        break;
    case GridContentAlign::SpaceAround:
        extra_gap = leftover / static_cast<float>(count);
        offset = extra_gap / 2.0f;
        break;
    case GridContentAlign::SpaceEvenly:
        extra_gap = leftover / static_cast<float>(count + 1);
        offset = extra_gap;
        break;
    }
    axis.step = axis.gap + extra_gap;
    for (int i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        axis.offsets[index] = offset;
        offset += axis.base[index] + axis.step;
    }
}

} // namespace tessera::ui::layout::detail
