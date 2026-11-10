// SPDX-License-Identifier: MIT

// Per-window render-scale state: adoption, recompute only on a DPI/monitor change, revision
// stability inside one ladder step, and rejection of invalid metrics.

#include "tessera/ui/scale/window_render_scale.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <iostream>
#include <limits>

namespace {

using tessera::hal::WindowMetrics;
using tessera::ui::scale::kRenderScaleLadder;
using tessera::ui::scale::RenderScaleChange;
using tessera::ui::scale::RenderScaleError;
using tessera::ui::scale::RenderScaleTransition;
using tessera::ui::scale::WindowRenderScale;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scale_state: " << message << "\n";
    }
    return condition;
}

WindowMetrics metrics_with_scale(float dpi_scale) {
    WindowMetrics metrics;
    metrics.logical_width = 1280;
    metrics.logical_height = 720;
    metrics.physical_width = 1280;
    metrics.physical_height = 720;
    metrics.dpi_scale = dpi_scale;
    return metrics;
}

} // namespace

int main() {
    WindowRenderScale state;
    if (!expect(!state.initialized(), "a fresh state is not initialized") ||
        !expect(state.revision() == 0, "a fresh state has revision 0") ||
        !expect(state.ladder_index() == -1, "a fresh state has no ladder index") ||
        !expect(state.recompute_count() == 0, "a fresh state has not recomputed") ||
        !expect(state.rejected_count() == 0, "a fresh state has rejected nothing") ||
        !expect(state.render_scale() == kRenderScaleLadder.front(),
                "a fresh state reports the ladder floor")) {
        return 1;
    }

    // Adoption: the first accepted snapshot replaces the sentinel revision.
    const std::expected<RenderScaleTransition, RenderScaleError> adopted =
        state.observe(metrics_with_scale(1.75f));
    if (!expect(adopted.has_value(), "valid metrics are accepted") ||
        !expect(adopted->change == RenderScaleChange::Adopted, "the first snapshot is Adopted") ||
        !expect(adopted->ladder_changed, "adoption changes the ladder step") ||
        !expect(adopted->device_scale == 1.75f, "adoption keeps the raw device scale") ||
        !expect(adopted->render_scale == 2.0f, "1.75 quantizes up to 2.0") ||
        !expect(adopted->ladder_index == 3, "2.0 is ladder index 3") ||
        !expect(adopted->revision == 1, "adoption bumps the revision") ||
        !expect(state.recompute_count() == 1, "adoption quantizes once") ||
        !expect(state.initialized(), "adoption initializes the state")) {
        return 2;
    }

    // Frames: a resize or a repaint reports the same dpi scale, so nothing is recomputed. The
    // logical/physical size is deliberately different every frame.
    for (std::uint32_t frame = 0; frame < 120; ++frame) {
        WindowMetrics metrics = metrics_with_scale(1.75f);
        metrics.logical_width = 1280 + frame;
        metrics.physical_width = 1280 + frame;
        const std::expected<RenderScaleTransition, RenderScaleError> observed =
            state.observe(metrics);
        if (!expect(observed.has_value(), "a frame snapshot is accepted") ||
            !expect(observed->change == RenderScaleChange::None,
                    "an unchanged dpi scale reports no change") ||
            !expect(!observed->ladder_changed, "an unchanged dpi scale keeps the step") ||
            !expect(observed->revision == 1, "an unchanged dpi scale keeps the revision") ||
            !expect(observed->render_scale == 2.0f,
                    "an unchanged dpi scale reports the live step")) {
            return 3;
        }
    }
    if (!expect(state.recompute_count() == 1, "120 frame snapshots do not recompute") ||
        !expect(state.rejected_count() == 0, "frame snapshots reject nothing")) {
        return 4;
    }

    // A DPI change that stays inside one ladder step: the raw scale is republished, the step and
    // the revision are not, so no atlas is dropped.
    const std::expected<RenderScaleTransition, RenderScaleError> same_step =
        state.observe(metrics_with_scale(1.9f));
    if (!expect(same_step.has_value(), "a same-step dpi change is accepted") ||
        !expect(same_step->change == RenderScaleChange::DpiChange, "1.75 -> 1.9 is a DPI change") ||
        !expect(!same_step->ladder_changed, "1.75 -> 1.9 keeps the 2.0 step") ||
        !expect(same_step->device_scale == 1.9f, "the raw scale follows the metrics") ||
        !expect(same_step->render_scale == 2.0f, "the step stays 2.0") ||
        !expect(same_step->revision == 1, "a same-step change keeps the revision") ||
        !expect(state.recompute_count() == 2, "a changed raw scale does recompute")) {
        return 5;
    }

    // A DPI change that crosses a step bumps the revision; that is what invalidates raster work.
    const std::expected<RenderScaleTransition, RenderScaleError> stepped =
        state.observe(metrics_with_scale(1.25f));
    if (!expect(stepped.has_value(), "a cross-step dpi change is accepted") ||
        !expect(stepped->change == RenderScaleChange::DpiChange, "1.9 -> 1.25 is a DPI change") ||
        !expect(stepped->ladder_changed, "1.9 -> 1.25 changes the step") ||
        !expect(stepped->render_scale == 1.25f, "1.25 is a ladder value") ||
        !expect(stepped->ladder_index == 1, "1.25 is ladder index 1") ||
        !expect(stepped->revision == 2, "a cross-step change bumps the revision")) {
        return 6;
    }

    // Fractional re-queries inside 1.25 never mint a second revision (1.20 and 1.24 both snap to
    // 1.25), which is what keeps one atlas per distinct scale.
    const std::array<float, 2> kInsideStep{1.20f, 1.24f};
    for (const float dpi_scale : kInsideStep) {
        const std::expected<RenderScaleTransition, RenderScaleError> inside =
            state.observe(metrics_with_scale(dpi_scale));
        if (!expect(inside.has_value(), "a fractional re-query is accepted") ||
            !expect(inside->change == RenderScaleChange::DpiChange,
                    "a fractional re-query is a DPI change") ||
            !expect(!inside->ladder_changed, "a fractional re-query keeps the 1.25 step") ||
            !expect(inside->render_scale == 1.25f, "a fractional re-query reports 1.25") ||
            !expect(inside->revision == 2, "a fractional re-query keeps the revision")) {
            return 7;
        }
    }

    // Invalid metrics are rejected and leave the state untouched.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const std::array<float, 5> kInvalid{0.0f, -1.0f, nan, inf, -inf};
    const std::uint32_t revision_before = state.revision();
    const std::uint32_t recompute_before = state.recompute_count();
    std::uint32_t rejections = 0;
    for (const float dpi_scale : kInvalid) {
        const std::expected<RenderScaleTransition, RenderScaleError> rejected =
            state.observe(metrics_with_scale(dpi_scale));
        ++rejections;
        if (!expect(!rejected.has_value(), "an invalid dpi scale is rejected") ||
            !expect(rejected.error() == RenderScaleError::InvalidDeviceScale,
                    "an invalid dpi scale reports InvalidDeviceScale") ||
            !expect(state.rejected_count() == rejections, "every rejection is counted") ||
            !expect(state.revision() == revision_before, "a rejection keeps the revision") ||
            !expect(state.render_scale() == 1.25f, "a rejection keeps the live step") ||
            !expect(state.initialized(), "a rejection keeps the state initialized")) {
            return 8;
        }
    }
    if (!expect(state.recompute_count() == recompute_before, "a rejection does not recompute")) {
        return 9;
    }

    // A valid frame right after a rejected one is a no-op, not a spurious ladder change.
    const std::expected<RenderScaleTransition, RenderScaleError> recovered =
        state.observe(metrics_with_scale(1.24f));
    if (!expect(recovered.has_value(), "a valid frame after a rejection is accepted") ||
        !expect(recovered->change == RenderScaleChange::None,
                "a valid frame after a rejection reports no change") ||
        !expect(recovered->revision == revision_before,
                "a valid frame after a rejection keeps the revision")) {
        return 10;
    }

    // A monitor change re-quantizes even when the dpi scale is identical; it only invalidates when
    // the step actually changes.
    const std::uint32_t recompute_before_display = state.recompute_count();
    const std::expected<RenderScaleTransition, RenderScaleError> same_display =
        state.notify_display_change(metrics_with_scale(1.24f));
    if (!expect(same_display.has_value(), "a monitor change is accepted") ||
        !expect(same_display->change == RenderScaleChange::MonitorChange,
                "a monitor change reports MonitorChange") ||
        !expect(!same_display->ladder_changed, "a same-step monitor change keeps the step") ||
        !expect(same_display->revision == revision_before,
                "a same-step monitor change keeps the revision") ||
        !expect(state.recompute_count() == recompute_before_display + 1,
                "a monitor change re-quantizes once")) {
        return 11;
    }

    const std::expected<RenderScaleTransition, RenderScaleError> moved_display =
        state.notify_display_change(metrics_with_scale(1.5f));
    if (!expect(moved_display.has_value(), "a scaled monitor change is accepted") ||
        !expect(moved_display->change == RenderScaleChange::MonitorChange,
                "a scaled monitor change reports MonitorChange") ||
        !expect(moved_display->ladder_changed, "a scaled monitor change changes the step") ||
        !expect(moved_display->render_scale == 1.5f, "1.5 is the new step") ||
        !expect(moved_display->ladder_index == 2, "1.5 is ladder index 2") ||
        !expect(moved_display->revision == revision_before + 1,
                "a scaled monitor change bumps the revision")) {
        return 12;
    }
    const std::expected<RenderScaleTransition, RenderScaleError> rejected_display =
        state.notify_display_change(metrics_with_scale(0.0f));
    if (!expect(!rejected_display.has_value(),
                "a monitor change with an invalid scale is rejected") ||
        !expect(state.render_scale() == 1.5f, "a rejected monitor change keeps the live step")) {
        return 13;
    }

    // A monitor change can be the first snapshot too, and it still adopts.
    WindowRenderScale fresh_display;
    const std::expected<RenderScaleTransition, RenderScaleError> first_display =
        fresh_display.notify_display_change(metrics_with_scale(1.0f));
    if (!expect(first_display.has_value(), "a first monitor change is accepted") ||
        !expect(first_display->change == RenderScaleChange::Adopted,
                "the first snapshot is Adopted even from a monitor change") ||
        !expect(first_display->revision == 1, "the first snapshot bumps the revision")) {
        return 14;
    }

    // Invalid metrics before adoption leave the sentinel state intact.
    WindowRenderScale invalid_first;
    if (!expect(!invalid_first.observe(metrics_with_scale(nan)).has_value(),
                "an invalid first snapshot is rejected") ||
        !expect(!invalid_first.initialized(), "an invalid first snapshot does not initialize") ||
        !expect(invalid_first.revision() == 0, "an invalid first snapshot keeps revision 0") ||
        !expect(invalid_first.rejected_count() == 1, "an invalid first snapshot is counted")) {
        return 15;
    }

    // Scripted DPI script: revisions advance exactly on step changes, never on repeats.
    WindowRenderScale scripted;
    constexpr std::array<float, 8> kDpiScript{1.0f, 1.25f, 1.5f, 2.0f, 1.24f, 1.0f, 1.0f, 2.0f};
    constexpr std::array<float, 8> kExpectedScales{1.0f,  1.25f, 1.5f, 2.0f,
                                                   1.25f, 1.0f,  1.0f, 2.0f};
    constexpr std::array<std::uint32_t, 8> kExpectedRevisions{1, 2, 3, 4, 5, 6, 6, 7};
    for (std::size_t i = 0; i < kDpiScript.size(); ++i) {
        const std::expected<RenderScaleTransition, RenderScaleError> transition =
            scripted.observe(metrics_with_scale(kDpiScript[i]));
        if (!expect(transition.has_value(), "the scripted dpi sequence is accepted") ||
            !expect(transition->render_scale == kExpectedScales[i],
                    "the scripted step is expected") ||
            !expect(transition->revision == kExpectedRevisions[i],
                    "the scripted revision is expected") ||
            !expect(transition->ladder_changed ==
                        (kExpectedRevisions[i] != (i == 0 ? 0u : kExpectedRevisions[i - 1])),
                    "a revision bump and a ladder change always agree")) {
            return 16;
        }
    }
    if (!expect(scripted.recompute_count() == 7,
                "the scripted sequence recomputes on every change") ||
        !expect(scripted.rejected_count() == 0, "the scripted sequence rejects nothing")) {
        return 17;
    }

    std::cout << "scale_state: ok\n";
    return 0;
}
