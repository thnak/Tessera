// SPDX-License-Identifier: MIT

#include "scene_canvas_motion.hpp"
#include "scene_canvas_session.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

// The pipeline's config for a driver that repaints whole frames: no caches (a retained record
// would be keyed to damage this driver never computes), and interpolation on because the paint
// view is how an animated value reaches the paint pass.
ui::frame::FramePipelineConfig motion_config() {
    ui::frame::FramePipelineConfig config;
    config.interpolation = true;
    config.paint_cache = false;
    config.retain_layers = false;
    config.draw_list_cache_bytes = 0;
    config.cull_paint = false;
    return config;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

} // namespace

CanvasMotion::CanvasMotion()
    : clock_(),
      loop_(clock_),
      damage_(rendering::DamageConfig{rendering::rhi::Extent2D{1, 1}, 1}),
      pipeline_(animations_, loop_, tracker_, damage_, dirty_, motion_config()) {}

void CanvasMotion::sync(ui::Node& root, const ui::style::CompiledStyleSheet& sheet,
                        const ui::frame::FrameTick& tick, bool force) {
    pipeline_.set_style_keyframes(&sheet.keyframes());
    if (!sheet_declares_motion(sheet)) {
        return;
    }
    // A fresh selector tree per sync: the preview restyles from the live pseudo-class state, the
    // same read the paint pass makes, so the style a transition diffs against is the one painted.
    ui::style::SceneStyleTree tree(root);
    ui::paint::SceneStyleProvider provider(sheet, tree);
    (void)pipeline_.sync_style(root, provider, tick, force);
    pipeline_.sample_started_animations(root);
}

void CanvasMotion::prime(ui::Node& root, const ui::style::CompiledStyleSheet& sheet) {
    primed_ = true;
    animations_.set_reduced_motion(reduced_motion_);
    const ui::frame::FrameTick tick = pipeline_.tick(root, 0.0, 1.0);
    sync(root, sheet, tick, true);
    running_ = !animations_.empty() || !pipeline_.springs().empty();
    anchored_ = false;
}

CanvasMotionTick CanvasMotion::advance(ui::Node& root, const ui::style::CompiledStyleSheet& sheet,
                                       double now_seconds) {
    CanvasMotionTick out;
    // The first call records the baseline, so the reduced-motion setting the page passes with its
    // first tick is already in force when the animations the sheet declares start.
    if (!primed_) {
        prime(root, sheet);
    }
    // Idle time is not simulated time: an animation that starts after a pause begins at zero
    // elapsed, whatever the page's clock did in between.
    if (!anchored_ || !running_) {
        last_now_ = now_seconds;
    }
    double dt = now_seconds - last_now_;
    if (!(dt > 0.0)) {
        dt = 0.0;
    }
    dt = std::min(dt, kMaxFrameSeconds);
    last_now_ = now_seconds;
    out.dt = dt;

    clock_.advance(dt);
    (void)loop_.tick();
    animations_.set_reduced_motion(reduced_motion_);
    // Alpha 1: the paint view shows the sample of THIS tick. The preview's tick is the page's
    // frame, so there is no later tick to blend toward (`AppHost` blends because it renders between
    // fixed steps).
    const ui::frame::FrameTick tick = pipeline_.tick(root, dt, 1.0);
    out.samples = tick.animation.samples.size();
    sync(root, sheet, tick, false);
    ++ticks_;

    running_ = !animations_.empty() || !pipeline_.springs().empty();
    anchored_ = running_;
    out.running = running_;
    return out;
}

// The session's half of the driver (issue #603): thin forwards, kept here so the session unit stays
// inside the source-size limit and everything about motion reads in one file.
CanvasMotionTick CanvasSceneSession::animate(double now_seconds) {
    if (!started() || motion_ == nullptr) {
        return {};
    }
    return motion_->advance(session_->mutable_root(), session_->sheet(), now_seconds);
}

void CanvasSceneSession::set_reduced_motion(bool enabled) {
    reduced_motion_ = enabled;
    if (motion_ != nullptr) {
        motion_->set_reduced_motion(enabled);
    }
}

bool CanvasSceneSession::animating() const noexcept {
    return started() && motion_ != nullptr && motion_->running();
}

std::string canvas_tick_report_json(const CanvasMotionTick& tick, bool reduced_motion) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer,
                  "{\"tickVersion\":1,\"running\":%s,\"dtMs\":%.3f,\"samples\":%zu,"
                  "\"reducedMotion\":%s}",
                  tick.running ? "true" : "false", tick.dt * 1000.0, tick.samples,
                  reduced_motion ? "true" : "false");
    return std::string(buffer);
}

bool sheet_declares_motion(const ui::style::CompiledStyleSheet& sheet) {
    if (!sheet.keyframes().empty()) {
        return true;
    }
    for (const ui::style::CompiledRule& rule : sheet.rules()) {
        for (const ui::style::Declaration& declaration : rule.declarations) {
            if (starts_with(declaration.property, "transition") ||
                starts_with(declaration.property, "animation")) {
                return true;
            }
        }
    }
    return false;
}

} // namespace tessera::compiler::codegen::cli
