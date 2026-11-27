// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/diagnostics/log.hpp"
#include "tessera/diagnostics/metrics.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::diagnostics {

// What the offscreen target pool reports once per frame (rendering::gpu::OffscreenPoolStats, copied
// as plain numbers so this module does not depend on the renderer; docs/design/gpu/
// offscreen-layers.md section 5e).
struct OffscreenPressure {
    std::uint32_t targets = 0;         // live pooled targets
    std::uint32_t peak_live = 0;       // most targets live together this frame
    std::uint32_t max_targets = 0;     // the count cap
    std::uint64_t bytes = 0;           // bytes held
    std::uint64_t max_bytes = 0;       // the byte cap (0 = none)
    std::uint32_t exhausted = 0;       // requests a cap refused this frame
    std::uint64_t exhausted_total = 0; // ... since the pool was created
};

// Makes the pool's graceful fallback visible. When a cap refuses a request, the layer, mask, filter
// or glass node that asked paints without its effect - correct enough to keep running, wrong enough
// that nobody should find out by squinting. The reporter feeds a counter and two gauges into a
// MetricsRegistry every frame, and writes ONE warning per run (per reporter) through the Logger, so
// a scene that stays over the cap does not flood the log. Either collaborator may be null, which
// turns that half off; observe() allocates only for the one log message.
class OffscreenPressureReporter {
public:
    static constexpr const char* kCode = "TESSERA_GPU_OFFSCREEN_EXHAUSTED";

    OffscreenPressureReporter() = default;
    // Registers "gpu.offscreen.refused" (counter), "gpu.offscreen.peak_live" and
    // "gpu.offscreen.bytes" (gauges) in `metrics`. Both pointers are borrowed and must outlive
    // this.
    OffscreenPressureReporter(Logger* logger, MetricsRegistry* metrics);

    // Call once per frame, after the frame was recorded. Returns true when it wrote the warning.
    bool observe(const OffscreenPressure& pressure, std::uint64_t frame);

    [[nodiscard]] bool reported() const noexcept {
        return reported_;
    }
    // Requests refused since construction, as observed.
    [[nodiscard]] std::uint64_t refused() const noexcept {
        return refused_;
    }

private:
    Logger* logger_ = nullptr;
    MetricsRegistry* metrics_ = nullptr;
    std::size_t refused_metric_ = MetricsRegistry::kInvalidMetric;
    std::size_t peak_metric_ = MetricsRegistry::kInvalidMetric;
    std::size_t bytes_metric_ = MetricsRegistry::kInvalidMetric;
    std::uint64_t refused_ = 0;
    bool reported_ = false;
};

} // namespace tessera::diagnostics
