// SPDX-License-Identifier: MIT

#include "tessera/ui/input_pump.hpp"

#include <stdexcept>

namespace tessera::ui {

InputPump::InputPump(InputPumpConfig config)
    : config_(config),
      queue_(config.queue_capacity),
      sampler_(config.max_events_per_tick) {
    if (config_.max_events_per_tick == 0) {
        throw std::invalid_argument("input pump max_events_per_tick must be greater than zero");
    }
}

InputPumpStats InputPump::pump(Node& root, runtime::RuntimeLoop& loop) {
    InputPumpStats stats;

    sampler_.sample(queue_, config_.max_events_per_tick);
    for (const InputEvent& event : sampler_.events()) {
        ++stats.consumed;
        const InputDispatchResult result = router_.dispatch(root, event);
        if (result.dispatched) {
            ++stats.dispatched;
        } else {
            ++stats.dropped;
        }
    }
    stats.overflowed = queue_.overflowed();

    total_.consumed += stats.consumed;
    total_.dispatched += stats.dispatched;
    total_.dropped += stats.dropped;
    total_.overflowed = total_.overflowed || stats.overflowed;

    if (stats.consumed > 0) {
        loop.request_frame();
    }
    return stats;
}

void InputPump::reset() {
    InputEvent event;
    while (queue_.pop(event)) {
    }
    queue_.clear_overflow();
    sampler_.sample(queue_);
    router_.reset();
}

} // namespace tessera::ui
