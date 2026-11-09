// SPDX-License-Identifier: MIT

#include "tessera/ui/text/caret_blink.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace tessera::ui::text {
namespace {

bool valid_phase(double seconds) {
    return std::isfinite(seconds) && seconds > 0.0;
}

} // namespace

CaretBlink::CaretBlink(CaretBlinkConfig config)
    : config_(config) {
    if (!valid_phase(config_.visible_seconds) || !valid_phase(config_.hidden_seconds)) {
        throw std::invalid_argument("caret blink phases must be finite and greater than zero");
    }
}

void CaretBlink::clamp_time(double now_seconds) {
    if (std::isfinite(now_seconds) && now_seconds > now_) {
        now_ = now_seconds;
    }
}

bool CaretBlink::phase_visible(double now_seconds) const {
    const double period = config_.visible_seconds + config_.hidden_seconds;
    const double elapsed = now_seconds - anchor_;
    if (!(elapsed > 0.0)) {
        return true;
    }
    double phase = std::fmod(elapsed, period);
    if (phase < 0.0) {
        phase += period;
    }
    return phase < config_.visible_seconds;
}

bool CaretBlink::tick(double now_seconds) {
    clamp_time(now_seconds);
    if (!active_) {
        return false;
    }
    const bool visible = phase_visible(now_);
    if (visible == visible_) {
        return false;
    }
    visible_ = visible;
    return true;
}

bool CaretBlink::reset(double now_seconds) {
    clamp_time(now_seconds);
    anchor_ = now_;
    if (!active_) {
        return false;
    }
    const bool changed = !visible_;
    visible_ = true;
    return changed;
}

bool CaretBlink::set_active(bool active, double now_seconds) {
    clamp_time(now_seconds);
    if (active == active_) {
        return false;
    }
    active_ = active;
    anchor_ = now_;
    const bool visible = active;
    const bool changed = visible != visible_;
    visible_ = visible;
    return changed;
}

double CaretBlink::next_transition_seconds() const {
    if (!active_) {
        return std::numeric_limits<double>::infinity();
    }
    const double period = config_.visible_seconds + config_.hidden_seconds;
    const double elapsed = now_ - anchor_;
    const double cycles = elapsed > 0.0 ? std::floor(elapsed / period) : 0.0;
    const double phase = elapsed - cycles * period;
    if (phase < config_.visible_seconds) {
        return anchor_ + cycles * period + config_.visible_seconds;
    }
    return anchor_ + (cycles + 1.0) * period;
}

} // namespace tessera::ui::text
