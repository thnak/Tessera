// SPDX-License-Identifier: MIT
//
// The window driver's half of the offscreen layer pool (docs/design/gpu/offscreen-layers.md section
// 5e): it samples the pool after every frame, so a cap that makes a layer, mask, filter or glass
// node paint without its effect is a counter and one logged warning instead of a silent difference,
// and it frees the pool's idle targets when the scene has settled, so a static UI holds no layer
// memory.

#include "tessera/diagnostics/offscreen_pressure.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "window_impl.hpp"

#include <algorithm>

namespace tessera::app {

namespace hal = tessera::hal;

void WindowHost::Impl::init_offscreen_diagnostics() {
    own_logger.add_sink(stderr_sink);
    diagnostics::Logger* const logger = options.logger != nullptr ? options.logger : &own_logger;
    offscreen_pressure = diagnostics::OffscreenPressureReporter(logger, options.metrics);
}

void WindowHost::Impl::observe_offscreen() {
    rendering::gpu::Batched2DRenderer* const batched = renderer();
    if (batched == nullptr) {
        return;
    }
    const rendering::gpu::OffscreenPoolStats pool = batched->offscreen_stats();
    stats.offscreen_peak_live = std::max(stats.offscreen_peak_live, pool.peak_live_max);
    stats.offscreen_bytes = pool.bytes;
    stats.offscreen_refused = pool.exhausted_total;
    if (pool.peak_live > 0) {
        offscreen_last_use_seconds = hal::steady_now_seconds();
    }
    diagnostics::OffscreenPressure pressure;
    pressure.targets = pool.targets;
    pressure.peak_live = pool.peak_live;
    pressure.max_targets = pool.max_targets;
    pressure.bytes = pool.bytes;
    pressure.max_bytes = pool.max_bytes;
    pressure.exhausted = pool.exhausted;
    pressure.exhausted_total = pool.exhausted_total;
    (void)offscreen_pressure.observe(pressure, stats.frames);
}

void WindowHost::Impl::trim_idle_offscreen(double now_seconds) {
    rendering::gpu::Batched2DRenderer* const batched = renderer();
    if (batched == nullptr || group == nullptr || group->window_count() != 1 ||
        stats.offscreen_bytes == 0 ||
        now_seconds - offscreen_last_use_seconds < detail::kOffscreenIdleSeconds) {
        return;
    }
    // A non-blocking look at the frames this window has in flight: the GPU must be done with every
    // pooled target before it is destroyed.
    for (const auto& commands : in_flight) {
        if (commands != nullptr && !commands->is_complete()) {
            return;
        }
    }
    batched->trim_offscreen_targets(true);
    stats.offscreen_bytes = batched->offscreen_stats().bytes;
}

} // namespace tessera::app
