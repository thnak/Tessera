// SPDX-License-Identifier: MIT
//
// The GPU frame-time signal for adaptive quality (docs/design/performance.md section 6.1). Three
// pure rules are fixed here, with no GPU, window or wall clock:
//   - the timestamp interval arithmetic: ticks x timestampPeriod, a 64-bit counter wrap, and the
//     zero/absurd/unusable-period cases that must fall back;
//   - the capability gate: only a device probe whose three facts all hold makes a GPU sample
//     eligible (a fake capability is enough to test this);
//   - the source-selection rule and its diagnostics: a present and plausible GPU interval is what
//     the controller compares, otherwise it sees the CPU frame time, and FrameDiagnostics records
//     which one ran.
// What cannot be tested here: the real Vulkan query pair (pool creation, recording, a completed
// submission's readback) needs a Vulkan device; the null backend cannot express a query at all, so
// its fallback is exactly the unusable-capability default proved below.

#include "adaptive_quality.hpp"
#include "tessera/app/host.hpp"
#include "tessera/rendering/rhi/timestamp.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/config.hpp"
#include "tessera/runtime/diagnostics.hpp"
#include "tessera/runtime/frame_time_source.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>

namespace {

using tessera::app::AppHost;
using tessera::app::HostConfig;
using tessera::rendering::rhi::GpuTimestampCapability;
using tessera::rendering::rhi::kMaxPlausibleTimestampIntervalMs;
using tessera::rendering::rhi::timestamp_delta_ticks;
using tessera::rendering::rhi::timestamp_interval_ms;
using tessera::runtime::FrameTimeSource;
using tessera::runtime::kAdaptiveQualityDowngradeMisses;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::runtime::select_frame_time;
using tessera::ui::Node;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "gpu_frame_time: " << message << "\n";
    }
    return condition;
}

bool close_to(double lhs, double rhs) {
    return std::fabs(lhs - rhs) < 1e-9;
}

// --- 1. Interval arithmetic -------------------------------------------------------------------

bool timestamp_interval_arithmetic() {
    bool ok = true;
    // 999,000 ticks at 1 ns/tick = 0.999 ms.
    const auto plain = timestamp_interval_ms(1000, 1'000'000, 1.0);
    ok = expect(plain.has_value() && close_to(*plain, 0.999),
                "ticks are scaled by the device timestamp period") &&
         ok;
    // 500,000 ticks at 2 ns/tick = 1 ms.
    const auto period_two = timestamp_interval_ms(0, 500'000, 2.0);
    ok = expect(period_two.has_value() && close_to(*period_two, 1.0),
                "a non-unit timestamp period is applied") &&
         ok;

    // A 64-bit counter wrap: begin near UINT64_MAX, end just past zero. The unsigned subtraction is
    // defined modulo 2^64, so the wrapped distance is 9 ticks, never a negative interval.
    const std::uint64_t begin = std::numeric_limits<std::uint64_t>::max() - 5;
    const auto wrapped_ticks = timestamp_delta_ticks(begin, 3);
    ok = expect(wrapped_ticks.has_value() && *wrapped_ticks == 9,
                "a counter wrap reports the wrapped tick distance") &&
         ok;
    const auto wrapped_ms = timestamp_interval_ms(begin, 3, 1'000'000.0);
    ok = expect(wrapped_ms.has_value() && close_to(*wrapped_ms, 9.0),
                "a wrapped interval converts with the same period") &&
         ok;

    // Zero delta: the GPU measured nothing between the two writes.
    ok = expect(!timestamp_delta_ticks(42, 42).has_value(), "an equal pair has no delta") && ok;
    ok = expect(!timestamp_interval_ms(42, 42, 1.0).has_value(),
                "a zero delta is not a frame time") &&
         ok;

    // Absurd delta: a stale pair (or a bogus period) must not reach the controller.
    const auto absurd = timestamp_interval_ms(0, 1'000'000, 1'000'000'000.0);
    ok = expect(!absurd.has_value(), "an interval above the plausibility bound falls back") && ok;
    ok = expect(kMaxPlausibleTimestampIntervalMs > 1000.0,
                "the plausibility bound admits a slow but real frame") &&
         ok;

    // Unusable periods: zero, negative, NaN and infinity.
    for (const double period : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity()}) {
        ok = expect(!timestamp_interval_ms(0, 1'000, period).has_value(),
                    "a zero, negative or non-finite period falls back") &&
             ok;
    }
    return ok;
}

// --- 2. Capability gate -----------------------------------------------------------------------

bool capability_gates_the_source() {
    bool ok = true;
    GpuTimestampCapability capability;
    ok = expect(!capability.usable(),
                "the default probe is the CPU fallback every backend starts from") &&
         ok;

    capability.queries_supported = true;
    capability.queue_supported = true;
    capability.period_ns = 1.0;
    ok = expect(capability.usable(), "all three probe facts make timestamps usable") && ok;

    // Each missing fact alone keeps the fallback: no consumer may guess past the probe.
    GpuTimestampCapability no_queries = capability;
    no_queries.queries_supported = false;
    ok = expect(!no_queries.usable(), "no timestamp queries on the queue family falls back") && ok;
    GpuTimestampCapability no_graphics = capability;
    no_graphics.queue_supported = false;
    ok = expect(!no_graphics.usable(), "a queue family that cannot record the frame falls back") &&
         ok;
    GpuTimestampCapability no_period = capability;
    no_period.period_ns = 0.0;
    ok = expect(!no_period.usable(), "a zero timestamp period falls back") && ok;
    GpuTimestampCapability nan_period = capability;
    nan_period.period_ns = std::numeric_limits<double>::quiet_NaN();
    ok = expect(!nan_period.usable(), "a non-finite timestamp period falls back") && ok;

    // The selection rule with the fake capability: available -> GPU, unavailable -> CPU.
    const auto choose = [](const GpuTimestampCapability& gate) {
        const std::optional<double> gpu = gate.usable() ? std::optional<double>{5.0} : std::nullopt;
        return select_frame_time(20.0, gpu);
    };
    const tessera::runtime::FrameTimeSample gpu = choose(capability);
    ok = expect(gpu.source == FrameTimeSource::Gpu && close_to(gpu.frame_time_ms, 5.0),
                "a usable capability selects the GPU sample") &&
         ok;
    for (const GpuTimestampCapability* unavailable :
         {&no_queries, &no_graphics, &no_period, &nan_period}) {
        const tessera::runtime::FrameTimeSample cpu = choose(*unavailable);
        ok = expect(cpu.source == FrameTimeSource::Cpu && close_to(cpu.frame_time_ms, 20.0),
                    "an unusable capability keeps the CPU sample") &&
             ok;
    }

    // A present-but-unusable GPU number also falls back; only the optional's presence is not
    // enough.
    for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        const tessera::runtime::FrameTimeSample sample = select_frame_time(20.0, bad);
        ok = expect(sample.source == FrameTimeSource::Cpu && close_to(sample.frame_time_ms, 20.0),
                    "a zero, negative or non-finite GPU sample falls back") &&
             ok;
    }
    return ok;
}

// --- 3. Host wiring and diagnostics -----------------------------------------------------------

bool host_records_the_source_and_feeds_the_gpu_sample() {
    Node root(1);
    Node& card = root.add_child(std::make_unique<Node>(2));
    card.set_class_name("card");
    auto sheet = compile_stylesheet(R"(
.card { width: 100px; height: 40px; background-color: #1e293b; }
)");

    ManualClock clock;
    RuntimeLoop loop(clock);
    HostConfig config;
    config.width = 320.0f;
    config.height = 200.0f;
    config.adaptive_quality.frame_budget_ms = 16.0;
    config.adaptive_quality.render_scale = 1.0f;
    config.adaptive_quality.min_render_scale = 0.5f;
    AppHost host(root, std::move(sheet), loop, config);

    bool ok = true;
    ok = expect(host.frame_diagnostics().frame_time_source() == FrameTimeSource::Cpu,
                "a fresh host has not observed a GPU sample") &&
         ok;
    ok = expect(host.frame_diagnostics().gpu_frame_time_samples() == 0 &&
                    host.frame_diagnostics().cpu_frame_time_samples() == 0,
                "no observation has been recorded yet") &&
         ok;

    // The CPU time is over budget (20 ms > 16 ms) while the GPU interval is inside it (5 ms). If
    // the controller compared the CPU number it would downgrade after the whole miss run; it must
    // not, which proves the GPU sample is the one it sees.
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityDowngradeMisses; ++frame) {
        host.observe_frame_time_sample(20.0, 5.0);
    }
    ok = expect(host.adaptive_quality().quality_step == 0,
                "the controller compares the GPU interval, not the CPU cost") &&
         ok;
    ok = expect(host.frame_diagnostics().frame_time_source() == FrameTimeSource::Gpu,
                "diagnostics report the GPU source") &&
         ok;
    ok =
        expect(host.frame_diagnostics().gpu_frame_time_samples() == kAdaptiveQualityDowngradeMisses,
               "every GPU observation is counted") &&
        ok;
    ok = expect(host.frame_diagnostics().cpu_frame_time_samples() == 0,
                "no CPU observation was counted") &&
         ok;

    // Without a completed submission interval the same over-budget CPU time drives the controller
    // and the fallback is counted, so a stale or missing timestamp never passes for a GPU number.
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityDowngradeMisses; ++frame) {
        host.observe_frame_time_sample(20.0, std::nullopt);
    }
    // The scene has no blurred shadow, so the effects position is stepped over: position 2.
    ok = expect(host.adaptive_quality().quality_step == 2,
                "with no GPU sample the CPU frame time drives the controller") &&
         ok;
    ok = expect(host.frame_diagnostics().frame_time_source() == FrameTimeSource::Cpu,
                "diagnostics report the CPU fallback") &&
         ok;
    ok =
        expect(host.frame_diagnostics().cpu_frame_time_samples() == kAdaptiveQualityDowngradeMisses,
               "every fallback observation is counted") &&
        ok;

    // The original CPU-only entry point keeps the identical behaviour and source.
    host.observe_frame_time_ms(10.0);
    ok = expect(host.frame_diagnostics().frame_time_source() == FrameTimeSource::Cpu,
                "the CPU-only entry point reports the CPU source") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = timestamp_interval_arithmetic() && ok;
    ok = capability_gates_the_source() && ok;
    ok = host_records_the_source_and_feeds_the_gpu_sample() && ok;
    if (!ok) {
        std::cerr << "gpu_frame_time: FAILED\n";
        return 1;
    }
    std::cout << "gpu_frame_time: ok\n";
    return 0;
}
