// SPDX-License-Identifier: MIT

#include "tessera/runtime/clock.hpp"

#include <cmath>
#include <stdexcept>

namespace tessera::runtime {
namespace {

bool is_valid_seconds(double value) {
    return std::isfinite(value) && value >= 0.0;
}

} // namespace

ManualClock::ManualClock(double start_seconds)
    : now_(start_seconds) {
    if (!is_valid_seconds(start_seconds)) {
        throw std::invalid_argument("clock start must be finite and non-negative");
    }
}

double ManualClock::now_seconds() const {
    return now_;
}

void ManualClock::advance(double seconds) {
    if (!is_valid_seconds(seconds)) {
        throw std::invalid_argument("clock advance must be finite and non-negative");
    }
    now_ += seconds;
}

} // namespace tessera::runtime
