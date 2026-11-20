// SPDX-License-Identifier: MIT

#include "scene_layout.hpp"
#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/text_wrap.hpp"
#include "tessera/ui/style/safe_area_style.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

// Formats `<value>px` into caller-owned storage: `ComputedStyle` owns its strings, and the layout
// input owns its computed styles, so nothing here borrows from this frame.
std::string pixels(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%gpx", static_cast<double>(value));
    return std::string(buffer);
}

// One layout tree of computed styles, the same shape `app::AppHost` builds for a frame
// (src/app/host.cpp, build_layout_input): the style comes from the compiled sheet through the
// captured scene tree, a text payload becomes the measured text, and an inline-span child is
// skipped because it is a run of the parent's text, not a box.
//
// **The SafeArea step is the app's, literally.** `apply_safe_area_style()` is the same function
// `AppHost::build_layout_input()` calls, with the same `WindowMetrics*`, so a `SafeArea` inside a
// `book` posture pads by `hal::resolve_safe_area()`'s answer and a full-occlusion hinge keeps the
// larger side. There is no preview-only branch: the only difference between the two callers is who
// filled the metrics in (an OS versus a simulated posture).
ui::layout::LayoutInput build_layout_input(const ui::Node& node, ui::style::SceneStyleTree& tree,
                                           const ui::style::CompiledStyleSheet& sheet,
                                           const hal::WindowMetrics& metrics) {
    ui::layout::LayoutInput input;
    input.id = node.id();
    input.style = tree.compute(sheet, node);
    (void)ui::style::apply_safe_area_style(input.style, node, &metrics);
    const auto& paint = node.paint_data();
    const bool text_payload = paint.has_value() && paint->kind == ui::PaintKind::Text;
    if (text_payload) {
        input.text = paint->text;
        // Layout's text-fit fact must use paint's own wrap decision (issue #490).
        input.text_wrap = ui::paint_wraps(node.element_kind(), *paint);
    }
    for (const auto& child : node.children()) {
        if (text_payload && paint->has_span(child->id())) {
            continue;
        }
        input.children.push_back(build_layout_input(*child, tree, sheet, metrics));
    }
    return input;
}

} // namespace

bool same_insets(const hal::WindowInsets& a, const hal::WindowInsets& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool same_cutouts(const hal::CutoutInfo& a, const hal::CutoutInfo& b) noexcept {
    if (a.count != b.count || !same_insets(a.waterfall_insets, b.waterfall_insets)) {
        return false;
    }
    for (std::size_t i = 0; i < hal::kMaxCutouts; ++i) {
        const hal::WindowRect& ra = a.bounds[i];
        const hal::WindowRect& rb = b.bounds[i];
        if (ra.x != rb.x || ra.y != rb.y || ra.width != rb.width || ra.height != rb.height) {
            return false;
        }
    }
    return true;
}

bool SceneMetrics::operator==(const SceneMetrics& other) const noexcept {
    return width == other.width && height == other.height && dpi_scale == other.dpi_scale &&
           posture == other.posture && same_insets(insets, other.insets) &&
           same_cutouts(cutouts, other.cutouts);
}

hal::WindowMetrics simulated_metrics(const SceneMetrics& metrics) noexcept {
    hal::WindowMetrics snapshot;
    const auto whole = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::max(value, 0.0f)));
    };
    snapshot.logical_width = whole(metrics.width);
    snapshot.logical_height = whole(metrics.height);
    snapshot.dpi_scale = metrics.dpi_scale > 0.0f ? metrics.dpi_scale : 1.0f;
    // The posture is data, exactly like the size: `fold_info_for` centres the band in this extent,
    // so the hinge rectangle and the window rectangle can never disagree.
    snapshot.fold =
        hal::fold_info_for(metrics.posture, snapshot.logical_width, snapshot.logical_height);
    // The skin's safe area, in the same two fields a device fills (issue #171). `resolve_safe_area`
    // raises the `SafeArea` padding from `insets` and `cutout_insets` from `cutouts`, so a
    // previewed phone pads its content exactly as the phone would - the skin changes the layout,
    // not only the picture around it (`designer-studio-v1.md` section 5.1).
    snapshot.insets = metrics.insets;
    snapshot.cutouts = metrics.cutouts;
    return snapshot;
}

SceneLayoutResult layout_scene(ui::Node& root, const ui::style::CompiledStyleSheet& sheet,
                               ui::layout::LayoutEngine& engine, const SceneMetrics& metrics,
                               std::shared_ptr<const text::TextMeasurer> measurer) {
    SceneLayoutResult result;
    result.metrics = simulated_metrics(metrics);
    // The window rectangle is applied to the layout input rather than to the node's inline style:
    // an inline declaration borrows its text from the caller, and a layout pass must not leave the
    // tree pointing at storage that dies with the call.
    ui::style::SceneStyleTree tree(root);
    ui::layout::LayoutInput input = build_layout_input(root, tree, sheet, result.metrics);
    input.style.set("width", pixels(metrics.width));
    input.style.set("height", pixels(metrics.height));

    ui::layout::LayoutViewport viewport;
    viewport.width = metrics.width;
    viewport.height = metrics.height;
    viewport.dpi_scale = result.metrics.dpi_scale;

    // The caller's engine may already carry a measurer of its own (a session that keeps one across
    // frames); only a null `measurer` leaves the engine's own choice in place. Constructing the
    // engine here instead would throw away a session's warm text-measure cache every frame.
    ui::layout::LayoutResult laid_out;
    if (measurer != nullptr) {
        ui::layout::LayoutEngine measured(measurer);
        laid_out = measured.layout(input, viewport);
    } else {
        laid_out = engine.layout(input, viewport);
    }
    if (laid_out.boxes.empty()) {
        result.error = "layout produced no boxes";
        return result;
    }
    const ui::layout::LayoutApplyResult applied = ui::layout::apply_layout(laid_out, root);
    result.applied = applied.applied;
    result.nodes = laid_out.boxes.size();
    return result;
}

} // namespace tessera::compiler::codegen::cli
