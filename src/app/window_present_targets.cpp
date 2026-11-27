// SPDX-License-Identifier: MIT
//
// The window frame's paint target, split out of window.cpp for its size budget (god-type audit
// slice 11). The host used to inline three unrelated reasons to render offscreen - adaptive render
// scale, glass, and the S8 4x presentation - into one frame branch; they share one policy decision
// (which target, at what extent, with what sample count), which is what this file owns.

#include "tessera/ui/scene/node.hpp"
#include "window_impl.hpp"

namespace tessera::app {

bool WindowHost::Impl::ensure_scaled_target(std::size_t frame_slot, rhi::Extent2D extent,
                                            std::uint32_t sample_count) {
    if (swapchain == nullptr || device == nullptr) {
        return false;
    }
    bool fallback = false;
    rhi::RenderTarget* target = scaled_targets.ensure(
        *device, frame_slot, extent, swapchain->format(),
        detail::frame_clear_color(translucent_presentation), sample_count, fallback);
    if (fallback) {
        ++stats.scale_fallbacks;
    }
    return target != nullptr;
}

std::optional<WindowHost::Impl::PaintTarget>
WindowHost::Impl::prepare_paint_target(std::size_t frame_slot, const rhi::Extent2D& presented,
                                       const ui::scale::AdaptiveTargetExtent& adaptive) {
    if (swapchain == nullptr || device == nullptr) {
        return std::nullopt;
    }
    const rhi::Color clear_color = detail::frame_clear_color(translucent_presentation);
    PaintTarget choice;
    choice.msaa = presentation_sample_count_ > 1;

    // Adaptive render scale (docs/design/performance.md section 6, step 2): the scene renders into
    // a target of the presented pixel grid times the controller's renderScale, and the present
    // resamples it up. A backend whose swapchain images cannot be resampled keeps the direct path
    // instead of dropping frames; the capability is fixed for this swapchain generation.
    choice.scaled = adaptive.scaled && scaled_targets.resample_supported() &&
                    ensure_scaled_target(frame_slot, adaptive.extent,
                                         choice.msaa ? presentation_sample_count_ : 1U);

    // A 4x presentation always renders offscreen: the 4-sample pipeline cannot render into the
    // 1-sample swapchain image, and a 4x target's resolved image is samplable but not a blit
    // source. A failed target allocation is a skipped frame, never a mismatched pass.
    if (choice.msaa && !choice.scaled &&
        !ensure_scaled_target(frame_slot, presented, presentation_sample_count_)) {
        return std::nullopt;
    }

    // A glass frame reads the target it paints into (the backdrop copy is a region of the frame
    // target), and a swapchain image is neither a copy source nor samplable (RHI 1.1/1.3). So a
    // scene with a glass node renders into the per-slot offscreen target at the presented extent
    // and the same submission resamples it into the acquired image - the adaptive path at content
    // scale 1, where a 1:1 blit is exact. Without the resample capability the frame paints
    // directly and the sink reports the glass draws as unsupported. With MSAA the frame target is
    // multisampled, which the backdrop copy cannot read, so glass reports its draws unsupported
    // for that generation (the documented bounded-cost fallback, glass-v1 section 8) and no extra
    // target is allocated for it. A layer `filter` is deliberately not asked about here: it blurs
    // on pooled layer targets and only composites into the frame, so `filter_subtree` keeps the
    // direct path (offscreen-layers.md section 5d).
    const bool glass_frame = !choice.scaled && !choice.msaa && root.glass_subtree() &&
                             scaled_targets.resample_supported() &&
                             ensure_scaled_target(frame_slot, presented);
    choice.offscreen = choice.scaled || glass_frame || choice.msaa;

    // The direct path's desc still describes the presented extent and the frame clear; only the
    // target pointer is filled by the caller once the swapchain image is acquired.
    choice.desc = detail::target_desc(presented, swapchain->format(), clear_color);
    if (choice.offscreen) {
        choice.target = scaled_targets.get(frame_slot);
        if (choice.target == nullptr) {
            return std::nullopt;
        }
        choice.desc = detail::target_desc(choice.target->extent(), swapchain->format(), clear_color,
                                          choice.target->sample_count());
    }
    return choice;
}

} // namespace tessera::app
