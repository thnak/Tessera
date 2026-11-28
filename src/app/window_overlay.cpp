// SPDX-License-Identifier: MIT
//
// The window driver's diagnostics overlay draw (issue #1024). One panel, one pass after the scene:
// the lines come from `OverlayModel` over a frame built by `build_window_overlay_frame`, and the
// text goes through the window's own text stack, so no second font path exists. The scene is
// already recorded when this runs, so the panel is the last thing in the draw list and sits on top.

#include "tessera/ui/text/text_shaper.hpp"
#include "window_impl.hpp"
#include "window_overlay.hpp"

#include <algorithm>
#include <vector>

namespace tessera::app {

void WindowHost::Impl::draw_diagnostics_overlay(bool full_repaint) {
    if (!options.runtime.diagnostics_overlay || renderer() == nullptr || text() == nullptr) {
        return;
    }
    constexpr float kPanelX = 8.0f;
    constexpr float kPanelY = 8.0f;
    constexpr float kPad = 8.0f;
    constexpr float kLineHeight = 18.0f;
    constexpr std::uint16_t kPixelSize = 14;

    WindowOverlayInput input;
    input.frame = stats.frames;
    input.scissors = overlay_scissors;
    input.full_redraw = full_repaint;
    input.budget_ms = options.runtime.frame_budget_ms;
    input.layer_cache_enabled = options.runtime.layer_cache_budget_mb > 0;
    const diagnostics::OverlayFrame frame = build_window_overlay_frame(
        input, overlay_meter, renderer()->retained_layer_stats(), host->layer_promotion_stats());
    const std::vector<std::string> lines = diagnostics::OverlayModel::build(frame, {}, 0);

    ui::text::TextStyle style;
    style.font_size = static_cast<float>(kPixelSize);
    std::vector<ui::text::ShapedRun> runs;
    runs.reserve(lines.size());
    float width = 0.0f;
    for (const std::string& line : lines) {
        runs.push_back(text()->shaper->shape(line, style));
        width = std::max(width, runs.back().advance);
    }
    const float height = kLineHeight * static_cast<float>(lines.size()) + kPad - 2.0f;
    renderer()->push_rounded_rect({kPanelX, kPanelY, width + 2.0f * kPad, height},
                                  rhi::rgba8(15, 23, 42, 232), 8.0f);
    float baseline = kPanelY + kPad + 12.0f;
    for (const ui::text::ShapedRun& run : runs) {
        text()->renderer->draw(*renderer(), run, kPanelX + kPad, baseline, kPixelSize,
                               rhi::rgba8(226, 232, 240));
        baseline += kLineHeight;
    }
}

} // namespace tessera::app
