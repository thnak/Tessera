// SPDX-License-Identifier: MIT
//
// The window's display-aware anti-aliasing policy (docs/design/display-aware-rendering.md section
// 6). It lives outside window.cpp and window_recovery.cpp because both call it - creation resolves
// the first policy once the text renderer exists, a metrics change re-resolves it - and because the
// timing rule is subtle enough to belong in one place.

#include "window_impl.hpp"

#include <cmath>

namespace tessera::app {

void WindowHost::Impl::apply_aa_policy(const hal::WindowMetrics& metrics) {
    // The device scale is the CSS -> device factor the paint pass snaps to (S3) and that the policy
    // quantizes for glyph rasterization. A platform that reports a nonsensical scale keeps 1:1
    // rather than dividing by zero somewhere downstream.
    device_scale =
        std::isfinite(metrics.dpi_scale) && metrics.dpi_scale > 0.0f ? metrics.dpi_scale : 1.0f;
    aa_policy = window_aa_policy(metrics, aa_options);
    // A re-application of the same policy is a no-op inside the renderer (hinting and scale are
    // compared), so a metrics event that moved nothing releases no atlas page.
    if (text != nullptr) {
        text->renderer->set_aa_policy(aa_policy);
    }
}

ui::scale::ContentScale
WindowHost::Impl::apply_frame_transform(rendering::gpu::Batched2DRenderer& batched,
                                        const ui::scale::AdaptiveTargetExtent& adaptive) const {
    // Scene geometry is authored in CSS px while the target is the window's physical pixel grid, so
    // the frame composes the two independent factors: the device scale and, on a downgraded frame,
    // the adaptive target/presented ratio. The same device_scale feeds border snapping and the AA
    // policy, so geometry and text can never be scaled by different values.
    const ui::scale::ContentScale transform = ui::scale::content_scale_for(
        device_scale, adaptive.content_scale_x, adaptive.content_scale_y);
    // At 1.0 the renderer's begin() already left the transform at identity, and setting it would
    // record no different commands; skipping the call keeps the full-quality path exactly as it
    // was.
    if (!transform.is_identity()) {
        batched.set_content_scale(transform.x, transform.y);
    }
    return transform;
}

} // namespace tessera::app
