// SPDX-License-Identifier: MIT

#include "present_cap.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::runtime {
namespace {

// The rate a policy presents at before any downgrade: the cap, clamped by the display.
double effective_rate(double refresh_hz, double configured_cap_hz) noexcept {
    if (configured_cap_hz <= 0.0) {
        return refresh_hz;
    }
    return refresh_hz > 0.0 ? std::min(configured_cap_hz, refresh_hz) : configured_cap_hz;
}

} // namespace

double adaptive_present_cap_hz(double refresh_hz, double configured_cap_hz,
                               std::uint32_t quality_step) noexcept {
    // A decision the config loader never produces is treated as "uncapped", never as a negative
    // rate the pacer would reject.
    if (!std::isfinite(configured_cap_hz) || configured_cap_hz < 0.0) {
        configured_cap_hz = 0.0;
    }
    if (!std::isfinite(refresh_hz) || refresh_hz <= 0.0) {
        // No divisor exists: the cap is the configured one and the pacer falls back to deadlines.
        return configured_cap_hz;
    }
    if (quality_step == 0) {
        return configured_cap_hz;
    }
    double rate = effective_rate(refresh_hz, configured_cap_hz);
    for (std::uint32_t step = 0; step < quality_step; ++step) {
        // The next divisor strictly below the current rate: floor(refresh / rate) + 1. A rate that
        // is already a divisor (refresh/2) moves to the next one (refresh/3), never to itself.
        const double divisor = std::floor(refresh_hz / rate) + 1.0;
        const double next = refresh_hz / divisor;
        if (next < kAdaptivePresentCapFloorHz || next >= rate) {
            // The next divisor would go below the floor (or the rate cannot move): the cap holds.
            // It stays a real divisor rather than being clamped to a rate the display cannot hit,
            // so every value this function returns is either the configured cap or refresh/k.
            break;
        }
        rate = next;
    }
    return rate;
}

} // namespace tessera::runtime
