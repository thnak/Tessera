// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::ui {

struct InputPumpConfig {
    // Capacity of the SPSC queue shared with the platform translator.
    std::size_t queue_capacity = 256;
    // Hard bound on events consumed per pump() call; the rest stay queued for the next tick.
    std::size_t max_events_per_tick = 256;
};

// Counters for one pump() call; InputPump::total() accumulates them.
struct InputPumpStats {
    std::uint64_t consumed = 0;
    std::uint64_t dispatched = 0;
    // Consumed but not delivered: no hit target, no focused node, or a lifecycle event
    // (PointerCancel/Focus/Blur) that only updates router state.
    std::uint64_t dropped = 0;
    // True when the queue has overflowed since the last reset; the platform reports the loss.
    bool overflowed = false;
};

// UI-thread input pump: drains the bounded queue under a per-tick budget, coalesces adjacent
// pointer moves, dispatches through the InputRouter, and wakes a render-on-demand RuntimeLoop.
//
// No blocking, no I/O, and no allocation once the queue and sampler are warm (both preallocate in
// the constructor). Handlers run synchronously on the calling (UI) thread. The pump owns the
// queue the platform writes into; hand it to hal::Window::set_input_queue().
class InputPump {
public:
    explicit InputPump(InputPumpConfig config = {});

    InputPump(const InputPump&) = delete;
    InputPump& operator=(const InputPump&) = delete;
    InputPump(InputPump&&) = delete;
    InputPump& operator=(InputPump&&) = delete;

    [[nodiscard]] InputQueue& queue() {
        return queue_;
    }
    [[nodiscard]] const InputQueue& queue() const {
        return queue_;
    }
    [[nodiscard]] InputRouter& router() {
        return router_;
    }
    [[nodiscard]] const InputRouter& router() const {
        return router_;
    }

    // Consumes at most max_events_per_tick events, dispatches each through the router, and calls
    // loop.request_frame() when at least one event was consumed (so idle loops wake even when the
    // event is dropped by hit testing).
    InputPumpStats pump(Node& root, runtime::RuntimeLoop& loop);

    [[nodiscard]] InputPumpStats total() const {
        return total_;
    }

    // Drops queued events, clears the overflow flag, and resets capture/focus state.
    void reset();

private:
    InputPumpConfig config_;
    InputQueue queue_;
    InputSampler sampler_;
    InputRouter router_;
    InputPumpStats total_;
};

} // namespace tessera::ui
