// SPDX-License-Identifier: MIT

#include "tessera/hal/safe_area.hpp"
#include "tessera/ui/style/window_environment.hpp"

#include <algorithm>

namespace tessera::ui::style {

namespace {

float non_negative(float value) {
    return std::max(value, 0.0f);
}

// The segments a separating fold leaves, as the `segments_x * segments_y` grid the standard
// variables index. A hinge that overshoots the viewport leaves an empty segment, never a negative
// one.
void split_viewport(StandardEnvironmentInputs& inputs, const hal::FoldInfo& fold) {
    const float width = inputs.viewport_width;
    const float height = inputs.viewport_height;
    const hal::WindowRect& hinge = fold.hinge_bounds;
    if (!fold.separating) {
        return;
    }
    if (fold.orientation == hal::FoldOrientation::Vertical) {
        const float left = std::clamp(hinge.x, 0.0f, width);
        const float right = std::clamp(hinge.x + hinge.width, left, width);
        inputs.segments_x = 2;
        inputs.segments = {{0.0f, 0.0f, non_negative(left), height},
                           {right, 0.0f, non_negative(width - right), height}};
    } else if (fold.orientation == hal::FoldOrientation::Horizontal) {
        const float top = std::clamp(hinge.y, 0.0f, height);
        const float bottom = std::clamp(hinge.y + hinge.height, top, height);
        inputs.segments_y = 2;
        inputs.segments = {{0.0f, 0.0f, width, non_negative(top)},
                           {0.0f, bottom, width, non_negative(height - bottom)}};
    }
}

} // namespace

StyleEnvironment window_environment(const hal::WindowMetrics& metrics, float viewport_width,
                                    float viewport_height, std::optional<float> text_scale) {
    StandardEnvironmentInputs inputs;
    const hal::WindowInsets insets = hal::resolve_safe_area(metrics, hal::SafeAreaConfig{});
    inputs.safe_area_top = insets.top;
    inputs.safe_area_right = insets.right;
    inputs.safe_area_bottom = insets.bottom;
    inputs.safe_area_left = insets.left;
    inputs.text_scale = text_scale;
    inputs.viewport_width = non_negative(viewport_width);
    inputs.viewport_height = non_negative(viewport_height);
    split_viewport(inputs, metrics.fold);
    return standard_environment(inputs);
}

} // namespace tessera::ui::style
