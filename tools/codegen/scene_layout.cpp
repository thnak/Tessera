// SPDX-License-Identifier: MIT

#include "scene_layout.hpp"
#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/scene_style.hpp"

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
ui::layout::LayoutInput build_layout_input(const ui::Node& node, ui::style::SceneStyleTree& tree,
                                           const ui::style::CompiledStyleSheet& sheet) {
    ui::layout::LayoutInput input;
    input.id = node.id();
    input.style = tree.compute(sheet, node);
    const auto& paint = node.paint_data();
    const bool text_payload = paint.has_value() && paint->kind == ui::PaintKind::Text;
    if (text_payload) {
        input.text = paint->text;
    }
    for (const auto& child : node.children()) {
        if (text_payload && paint->has_span(child->id())) {
            continue;
        }
        input.children.push_back(build_layout_input(*child, tree, sheet));
    }
    return input;
}

} // namespace

SceneLayoutResult layout_scene(ui::Node& root, const ui::style::CompiledStyleSheet& sheet,
                               ui::layout::LayoutEngine& engine, float width, float height,
                               float dpi_scale) {
    SceneLayoutResult result;
    // The window rectangle is applied to the layout input rather than to the node's inline style:
    // an inline declaration borrows its text from the caller, and a layout pass must not leave the
    // tree pointing at storage that dies with the call.
    ui::style::SceneStyleTree tree(root);
    ui::layout::LayoutInput input = build_layout_input(root, tree, sheet);
    input.style.set("width", pixels(width));
    input.style.set("height", pixels(height));

    ui::layout::LayoutViewport viewport;
    viewport.width = width;
    viewport.height = height;
    viewport.dpi_scale = dpi_scale;

    const ui::layout::LayoutResult laid_out = engine.layout(input, viewport);
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
