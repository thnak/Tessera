// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <string>

// The canvas preview's animation driver (issue #603): the production animation runtime and
// `FramePipeline` (the same ones `AppHost` runs) over the session's live tree, advanced by the
// page's own frame timestamps.
//
// **Why it exists.** The canvas painted a fresh `SceneStyleTree` per frame and ran no animation
// runtime, so `:hover` changed style instantly, `@keyframes` never played and nothing asked the
// page for a second frame. This owns what `AppHost` owns for the same purpose - an
// `AnimationRuntime`, a `RuntimeLoop` and a `FramePipeline` - and nothing else: no damage, no
// redraw plan, no paint cache, because the preview repaints the whole target each frame it owes.
//
// **The clock is a value, not a wall clock.** `advance(now)` takes the timestamp the page got from
// `requestAnimationFrame`, feeds the difference to a `ManualClock` and ticks the loop and the
// pipeline once, like `AppHost::begin_frame(dt, alpha)`. The same sequence of timestamps therefore
// always produces the same values, which is what the C++ tests and the Node gate rely on.
//
// **What animates is what the native paint view animates**: opacity, colors and transforms (the
// paint-only samples `FramePipeline` captures); layout-affecting properties stay discrete, exactly
// as in the native runtime.
//
// **Idle means zero frames.** `running()` is true only while the runtime holds an entry (or a
// spring); the page asks for another frame while it is and stops when it is not. The first idle
// timestamp re-anchors the clock, so the gap between two interactions is never simulated time.
namespace tessera::compiler::codegen::cli {

// What one `advance()` did.
struct CanvasMotionTick {
    bool running = false;    // animations remain: the page owes another frame
    double dt = 0.0;         // simulated seconds this tick advanced (clamped)
    std::size_t samples = 0; // animated values the tick produced
};

class CanvasMotion {
public:
    // One frame longer than this is a hidden tab or a debugger pause, not time an animation should
    // consume: the same clamp the runtime loop's `max_frame_delta_ms` applies.
    static constexpr double kMaxFrameSeconds = 0.25;

    CanvasMotion();

    CanvasMotion(const CanvasMotion&) = delete;
    CanvasMotion& operator=(const CanvasMotion&) = delete;

    // `prefers-reduced-motion`: the runtime clamps durations so animations settle at once.
    void set_reduced_motion(bool enabled) {
        reduced_motion_ = enabled;
    }
    [[nodiscard]] bool reduced_motion() const noexcept {
        return reduced_motion_;
    }

    // Records the style baseline of a freshly evaluated tree and starts the animations its style
    // declares (`animation:`). Without it the first hover would be a first sighting, not a change,
    // and start no transition. `advance()` calls it on the first frame of an evaluation, after the
    // page has said whether motion is reduced; calling it earlier is allowed.
    void prime(ui::Node& root, const ui::style::CompiledStyleSheet& sheet);

    // Advances to `now_seconds` (the page's frame timestamp) and syncs style against the live
    // tree. Cheap when the sheet declares no transition, animation or spring: a tick of an empty
    // runtime.
    CanvasMotionTick advance(ui::Node& root, const ui::style::CompiledStyleSheet& sheet,
                             double now_seconds);

    [[nodiscard]] bool running() const noexcept {
        return running_;
    }
    // The animated paint values to hand the paint pass; empty when nothing animates.
    [[nodiscard]] ui::frame::InterpolationPaintView view() const {
        return pipeline_.interpolation_view();
    }
    [[nodiscard]] const ui::animation::AnimationRuntime& animations() const noexcept {
        return animations_;
    }
    // Ticks run so far, for the counters a test and the report read.
    [[nodiscard]] std::uint64_t ticks() const noexcept {
        return ticks_;
    }

private:
    void sync(ui::Node& root, const ui::style::CompiledStyleSheet& sheet,
              const ui::frame::FrameTick& tick, bool force);

    runtime::ManualClock clock_;
    runtime::RuntimeLoop loop_;
    ui::animation::AnimationRuntime animations_;
    ui::layout::DamageTracker tracker_;
    rendering::DamageAccumulator damage_;
    ui::DirtyPass dirty_;
    ui::frame::FramePipeline pipeline_;
    double last_now_ = 0.0;
    std::uint64_t ticks_ = 0;
    bool primed_ = false;
    bool anchored_ = false; // `last_now_` is a timestamp of this animation run
    bool running_ = false;
    bool reduced_motion_ = false;
};

// The report of one `tessera_canvas_tick`, as one JSON object on one line:
//
//   {"tickVersion":1,"running":true,"dtMs":16.667,"samples":1,"reducedMotion":false}
//
// `running` is the page's keep-alive: it requests another frame while true and stops when false, so
// a static scene presents nothing.
[[nodiscard]] std::string canvas_tick_report_json(const CanvasMotionTick& tick,
                                                  bool reduced_motion);

// True when `sheet` declares anything that can move: a `@keyframes` block or a `transition*` /
// `animation*` declaration. A sheet that does not leaves the style sync off, so a static scene's
// pixels are exactly what they were before this driver existed.
[[nodiscard]] bool sheet_declares_motion(const ui::style::CompiledStyleSheet& sheet);

} // namespace tessera::compiler::codegen::cli
