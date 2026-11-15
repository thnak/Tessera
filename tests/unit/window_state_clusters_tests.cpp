// SPDX-License-Identifier: MIT
//
// The two state clusters extracted from WindowHost::Impl by the god-type audit slice 9, tested
// directly instead of through a window:
//
//  - FrameTimeProbe: the capability gate and the source-selection rule, plus the counting facts the
//    host needs (a usable capability with no interval is a counted fallback; an unusable one is
//    not). Pure CPU, no device, no wall clock.
//  - ScaledTargets: one target per slot, reused while the decided extent lasts, recreated when the
//    extent or format changes, released on a new generation, and reporting a refusal as a value
//    rather than a throw.
//
// What cannot be tested here: the real Vulkan query pair. The null backend cannot express a
// timestamp query at all, which is why the unusable-capability default below is the null path too.

#include "frame_time_probe.hpp"
#include "scaled_targets.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/runtime/frame_time_source.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>

namespace {

using tessera::app::detail::FrameTimeObservation;
using tessera::app::detail::FrameTimeProbe;
using tessera::app::detail::ScaledTargets;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::TextureFormat;
using tessera::runtime::FrameTimeSource;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_state_clusters: " << message << "\n";
    }
    return condition;
}

// --- FrameTimeProbe ---------------------------------------------------------------------------

bool check_probe_defaults_to_cpu() {
    FrameTimeProbe probe;
    const FrameTimeObservation observation = probe.observe(16.0);
    return expect(!probe.capability_usable(), "a fresh probe has no usable capability") &&
           expect(observation.sample.source == FrameTimeSource::Cpu,
                  "an unusable capability selects the CPU time") &&
           expect(observation.sample.frame_time_ms == 16.0, "the CPU time is passed through") &&
           expect(!observation.counted_fallback(),
                  "an unusable capability is not a counted fallback");
}

bool check_probe_uses_a_published_interval() {
    FrameTimeProbe probe;
    probe.reset(true);
    probe.publish(8.0);
    if (!expect(probe.has_pending(), "publishing stores an interval")) {
        return false;
    }
    const FrameTimeObservation observation = probe.observe(16.0);
    return expect(observation.sample.source == FrameTimeSource::Gpu,
                  "a usable capability with a fresh interval selects the GPU time") &&
           expect(observation.sample.frame_time_ms == 8.0, "the GPU interval wins over the CPU") &&
           expect(observation.gpu_frame_time_ms.has_value() &&
                      *observation.gpu_frame_time_ms == 8.0,
                  "the raw interval is reported to the caller") &&
           expect(!probe.has_pending(), "observing consumes the interval") &&
           expect(!observation.counted_fallback(), "a GPU sample is not a fallback");
}

bool check_probe_counts_a_missing_interval_as_fallback() {
    FrameTimeProbe probe;
    probe.reset(true);
    // No publish: the first frames of a generation behave exactly like this.
    const FrameTimeObservation observation = probe.observe(16.0);
    return expect(observation.sample.source == FrameTimeSource::Cpu,
                  "a usable capability with no interval falls back to the CPU time") &&
           expect(
               observation.counted_fallback(),
               "that fallback is counted, so a reader can tell it from an unusable capability") &&
           expect(!observation.gpu_frame_time_ms.has_value(), "there was no interval to report");
}

bool check_probe_ignores_an_interval_when_unusable() {
    FrameTimeProbe probe;
    probe.reset(false);
    probe.publish(8.0);
    if (!expect(!probe.has_pending(), "an unusable capability drops a published interval")) {
        return false;
    }
    const FrameTimeObservation observation = probe.observe(16.0);
    return expect(observation.sample.source == FrameTimeSource::Cpu,
                  "an unusable capability keeps the CPU time even after a publish") &&
           expect(!observation.counted_fallback(), "and does not count a fallback");
}

bool check_probe_reset_drops_a_pending_interval() {
    FrameTimeProbe probe;
    probe.reset(true);
    probe.publish(8.0);
    // A device loss must not let a dead generation's interval reach the next one.
    probe.reset(false);
    const FrameTimeObservation observation = probe.observe(16.0);
    return expect(!probe.has_pending(), "reset drops the pending interval") &&
           expect(observation.sample.source == FrameTimeSource::Cpu,
                  "no stale GPU interval survives a device loss");
}

bool check_probe_consumes_only_once() {
    FrameTimeProbe probe;
    probe.reset(true);
    probe.publish(8.0);
    static_cast<void>(probe.observe(16.0));
    const FrameTimeObservation second = probe.observe(16.0);
    return expect(second.sample.source == FrameTimeSource::Cpu,
                  "one interval feeds exactly one observation") &&
           expect(second.counted_fallback(), "the second observation is the counted fallback");
}

// --- ScaledTargets ----------------------------------------------------------------------------

struct Fixture {
    std::unique_ptr<tessera::rendering::rhi::Device> device =
        tessera::rendering::null::create_null_device({});
};

constexpr Extent2D kSmall{320, 200};
constexpr Extent2D kLarge{640, 400};

bool check_scaled_is_inert_before_a_generation() {
    ScaledTargets targets;
    return expect(!targets.resample_supported(),
                  "a fresh generation does not support resampling") &&
           expect(targets.get(0) == nullptr, "no target is allocated up front") &&
           expect(targets.slot_count() == 2, "two frames in flight means two slots");
}

bool check_scaled_allocates_and_reuses() {
    Fixture fixture;
    ScaledTargets targets;
    targets.reset(true);
    bool fallback = true;
    auto* first = targets.ensure(*fixture.device, 0, kSmall, TextureFormat::R8G8B8A8Unorm,
                                 tessera::rendering::rhi::rgba8(0, 0, 0), fallback);
    if (!expect(first != nullptr, "a small target is created") ||
        !expect(!fallback, "a successful creation is not a fallback")) {
        return false;
    }
    fallback = true;
    auto* again = targets.ensure(*fixture.device, 0, kSmall, TextureFormat::R8G8B8A8Unorm,
                                 tessera::rendering::rhi::rgba8(0, 0, 0), fallback);
    return expect(again == first,
                  "the same extent reuses the image, so a held downgrade costs no allocation") &&
           expect(!fallback, "a reuse is not a fallback") &&
           expect(targets.extent(0).width == kSmall.width &&
                      targets.extent(0).height == kSmall.height,
                  "the allocated extent is recorded");
}

bool check_scaled_recreates_on_a_new_extent() {
    Fixture fixture;
    ScaledTargets targets;
    targets.reset(true);
    bool fallback = false;
    auto* small = targets.ensure(*fixture.device, 0, kSmall, TextureFormat::R8G8B8A8Unorm,
                                 tessera::rendering::rhi::rgba8(0, 0, 0), fallback);
    auto* large = targets.ensure(*fixture.device, 0, kLarge, TextureFormat::R8G8B8A8Unorm,
                                 tessera::rendering::rhi::rgba8(0, 0, 0), fallback);
    return expect(small != nullptr && large != nullptr, "both targets were created") &&
           expect(large != small, "a different extent allocates a new image") &&
           expect(targets.extent(0).width == kLarge.width, "the new extent replaces the old");
}

bool check_scaled_keeps_slots_independent() {
    Fixture fixture;
    ScaledTargets targets;
    targets.reset(true);
    bool fallback = false;
    auto* slot0 = targets.ensure(*fixture.device, 0, kSmall, TextureFormat::R8G8B8A8Unorm,
                                 tessera::rendering::rhi::rgba8(0, 0, 0), fallback);
    auto* slot1 = targets.ensure(*fixture.device, 1, kSmall, TextureFormat::R8G8B8A8Unorm,
                                 tessera::rendering::rhi::rgba8(0, 0, 0), fallback);
    // One image shared across two frames in flight would be a cross-submission read/write hazard.
    return expect(slot0 != nullptr && slot1 != nullptr, "both slots have a target") &&
           expect(slot0 != slot1, "each frame slot owns its own image");
}

bool check_scaled_reset_releases_every_generation_target() {
    Fixture fixture;
    ScaledTargets targets;
    targets.reset(true);
    bool fallback = false;
    static_cast<void>(targets.ensure(*fixture.device, 0, kSmall, TextureFormat::R8G8B8A8Unorm,
                                     tessera::rendering::rhi::rgba8(0, 0, 0), fallback));
    static_cast<void>(targets.ensure(*fixture.device, 1, kSmall, TextureFormat::R8G8B8A8Unorm,
                                     tessera::rendering::rhi::rgba8(0, 0, 0), fallback));
    // The images belong to the old device.
    targets.reset(false);
    return expect(targets.get(0) == nullptr && targets.get(1) == nullptr,
                  "reset releases every target of the old generation") &&
           expect(!targets.resample_supported(), "reset takes the new capability") &&
           expect(targets.extent(0).width == 0, "a released slot reports a zero extent");
}

bool check_scaled_rejects_an_empty_or_out_of_range_request() {
    Fixture fixture;
    ScaledTargets targets;
    targets.reset(true);
    bool fallback = true;
    auto* empty = targets.ensure(*fixture.device, 0, Extent2D{0, 0}, TextureFormat::R8G8B8A8Unorm,
                                 tessera::rendering::rhi::rgba8(0, 0, 0), fallback);
    if (!expect(empty == nullptr, "a zero extent is refused") ||
        !expect(!fallback, "a refused empty request is not a backend fallback")) {
        return false;
    }
    fallback = true;
    auto* out_of_range = targets.ensure(*fixture.device, 7, kSmall, TextureFormat::R8G8B8A8Unorm,
                                        tessera::rendering::rhi::rgba8(0, 0, 0), fallback);
    return expect(out_of_range == nullptr, "an out-of-range slot is refused") &&
           expect(targets.get(7) == nullptr, "and nothing is allocated for it");
}

} // namespace

int main() {
    bool ok = true;
    ok = check_probe_defaults_to_cpu() && ok;
    ok = check_probe_uses_a_published_interval() && ok;
    ok = check_probe_counts_a_missing_interval_as_fallback() && ok;
    ok = check_probe_ignores_an_interval_when_unusable() && ok;
    ok = check_probe_reset_drops_a_pending_interval() && ok;
    ok = check_probe_consumes_only_once() && ok;
    ok = check_scaled_is_inert_before_a_generation() && ok;
    ok = check_scaled_allocates_and_reuses() && ok;
    ok = check_scaled_recreates_on_a_new_extent() && ok;
    ok = check_scaled_keeps_slots_independent() && ok;
    ok = check_scaled_reset_releases_every_generation_target() && ok;
    ok = check_scaled_rejects_an_empty_or_out_of_range_request() && ok;
    if (!ok) {
        std::cerr << "window_state_clusters: FAILED\n";
        return 1;
    }
    std::cout << "window_state_clusters: all checks passed\n";
    return 0;
}
