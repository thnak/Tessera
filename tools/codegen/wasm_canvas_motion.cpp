// SPDX-License-Identifier: MIT

// The canvas module's animation clock export (issue #603): `tessera_canvas_tick`.
//
// **Why it is its own export and not part of `tessera_canvas_draw`.** A draw is a paint of the tree
// the session holds and takes no clock; the page's frame timestamp is the one input the animation
// runtime needs and the draw ABI has nowhere to put it. Keeping the clock separate also keeps the
// draw callable without advancing time (a repaint after a resize must not skip an animation ahead).
// The page calls tick, then draw, in one `requestAnimationFrame` callback.
//
// Like the other session exports it takes the session through `canvas_query_session()` and never
// creates or replaces it, and it answers a well-formed report in every state.

#include "scene_canvas_motion.hpp"
#include "scene_canvas_session.hpp"

#include <cstdlib>
#include <emscripten/emscripten.h>
#include <string_view>

namespace tessera::compiler::codegen::cli {

// Defined in `wasm_canvas_entry.cpp`, which owns the module's state.
[[nodiscard]] CanvasSceneSession* canvas_query_session();
[[nodiscard]] char* canvas_duplicate_out(std::string_view text);

} // namespace tessera::compiler::codegen::cli

extern "C" {

// Advances the running session's animation runtime to `now_ms`, the page's `requestAnimationFrame`
// timestamp in milliseconds, and syncs style so transitions and `@keyframes` start and move.
// `reduced_motion` is the page's `matchMedia("(prefers-reduced-motion: reduce)").matches` (0 or 1):
// animations settle at once while it is set.
//
// The report's `running` is the keep-alive: true means an animation holds the runtime and the page
// requests the next frame after drawing this one; false means idle, and the page requests nothing
// until an input changes the scene. A timestamp is a value, not a wall clock, so a script can drive
// a deterministic sequence through the module.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_tick(double now_ms, int reduced_motion) {
    namespace cli = tessera::compiler::codegen::cli;
    cli::CanvasMotionTick tick;
    cli::CanvasSceneSession* session = cli::canvas_query_session();
    if (session != nullptr && session->started()) {
        session->set_reduced_motion(reduced_motion != 0);
        tick = session->animate(now_ms / 1000.0);
    }
    return cli::canvas_duplicate_out(cli::canvas_tick_report_json(tick, reduced_motion != 0));
}

} // extern "C"
