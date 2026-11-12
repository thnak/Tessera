// SPDX-License-Identifier: MIT

#include "tessera/hal/refresh_rate.hpp"

#include <cmath>

namespace tessera::hal {

double sanitize_display_refresh_hz(double reported_hz) noexcept {
    if (!std::isfinite(reported_hz) || reported_hz < kMinDisplayRefreshHz ||
        reported_hz > kMaxDisplayRefreshHz) {
        return kUnknownRefreshHz;
    }
    return reported_hz;
}

double resolve_display_refresh_hz(double explicit_hz, double reported_hz) noexcept {
    // An explicit option is the caller's decision and is never second-guessed here: the window host
    // validates that it is finite and non-negative before this runs, and a non-zero value means the
    // caller knows better than the platform query. Only "left unset" falls back to the report.
    if (std::isfinite(explicit_hz) && explicit_hz > 0.0) {
        return explicit_hz;
    }
    return sanitize_display_refresh_hz(reported_hz);
}

double refresh_hz_from_display_mode(std::uint64_t dot_clock_hz, std::uint32_t htotal,
                                    std::uint32_t vtotal) noexcept {
    if (dot_clock_hz == 0 || htotal == 0 || vtotal == 0) {
        return kUnknownRefreshHz;
    }
    const double pixels_per_frame = static_cast<double>(htotal) * static_cast<double>(vtotal);
    return sanitize_display_refresh_hz(static_cast<double>(dot_clock_hz) / pixels_per_frame);
}

} // namespace tessera::hal
