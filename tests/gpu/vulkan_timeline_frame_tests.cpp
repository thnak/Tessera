// SPDX-License-Identifier: MIT
//
// Multi-frame synchronization on the swapchain path: one timeline semaphore paces two frames in
// flight (docs/design/gpu/frames.md section 5.1). A hidden window + Vulkan swapchain is driven
// through the deterministic single-iteration seam WindowHost::pump_once(); every painted frame
// signals the next timeline value, a slot becomes reusable when vkGetSemaphoreCounterValue reaches
// it (a query, never a wait), and the whole run - device creation, frames, teardown - stays
// validation-clean with synchronization validation on. Skips when the platform has no window
// backend/presentation surface or the validation layer is unavailable (without it the counter
// cannot prove anything).

#include "tessera/app/window.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::app::WindowDeviceState;
using tessera::app::WindowFrameStats;
using tessera::app::WindowHost;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

// The app driver keeps two frames in flight (app/window_impl.hpp); the swapchain exposes one slot
// per frame, so the reported overlap can never exceed it.
constexpr std::uint32_t kFramesInFlight = 2;
// More than the six frames the slice asks for, so a skipped tick still leaves a real multi-frame
// run.
constexpr std::uint32_t kTargetFrames = 8;
// A bounded pump budget: a busy GPU costs skipped iterations, never an unbounded wait.
constexpr std::uint32_t kMaxIterations = 300;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_timeline_frames: " << message << "\n";
    }
    return condition;
}

// A paint-data change marks paint/render dirty without touching style or layout, so each iteration
// plans a real frame instead of relying on a forced-frame loop the seam deliberately does not have.
void set_label(Node& label, std::uint32_t iteration) {
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = "Timeline frame " + std::to_string(iteration);
    label.set_paint_data(std::move(data));
}

} // namespace

int main() {
    Node root(1);
    auto label_ptr = std::make_unique<Node>(2);
    Node& label = root.add_child(std::move(label_ptr));
    label.set_element_kind("Text");
    set_label(label, 0);

    tessera::app::WindowOptions options;
    options.title = "Tessera timeline frames";
    options.width = 320;
    options.height = 200;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    options.validation = true;

    std::uint32_t presented = 0;
    std::uint32_t frames = 0;
    std::uint32_t slots_skipped = 0;
    std::uint64_t timeline_end = 0;
    std::uint32_t max_in_flight = 0;
    bool timeline_monotonic = true;
    bool timeline_never_ahead = true;
    bool device_ready = false;
    bool stalled = false;
    {
        WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"), options);
        if (!host.available()) {
            std::cout << "vulkan_timeline_frames: skipped (no window backend or presentation "
                         "surface)\n";
            return 0;
        }
        if (!tessera::rendering::vulkan::validation_active()) {
            std::cout << "vulkan_timeline_frames: skipped (validation layer unavailable)\n";
            return 0;
        }

        const WindowFrameStats& stats = host.stats();
        std::uint64_t previous_timeline = stats.timeline_value;
        for (std::uint32_t iteration = 0; iteration < kMaxIterations; ++iteration) {
            if (stats.presented >= kTargetFrames) {
                break;
            }
            set_label(label, iteration + 1);
            (void)host.pump_once();
            // The counter only reports values the GPU has reached, so it can trail the frames
            // submitted so far (that trailing gap is exactly the frames in flight), but it may
            // never move backwards and never claim more values than were submitted.
            if (stats.timeline_value < previous_timeline) {
                timeline_monotonic = false;
            }
            if (stats.timeline_value > stats.frames) {
                timeline_never_ahead = false;
            }
            if (stats.frames_in_flight > max_in_flight) {
                max_in_flight = stats.frames_in_flight;
            }
            previous_timeline = stats.timeline_value;
        }

        presented = stats.presented;
        frames = stats.frames;
        slots_skipped = stats.slots_skipped;
        timeline_end = stats.timeline_value;
        device_ready = stats.device_state == WindowDeviceState::Ready;
        stalled = stats.stalled;
    }

    bool ok = true;
    // 1. Frames flowed: presented grew, at least six frames were painted, the device stayed healthy
    // and no frame failed (the bounded run did not stall).
    ok = expect(presented >= 6, "at least six frames were presented") && ok;
    ok = expect(frames >= 6, "at least six frames were painted and submitted") && ok;
    ok = expect(device_ready, "the run reports a healthy device (no device loss)") && ok;
    ok = expect(!stalled, "no frame failed: the run did not stall") && ok;

    // 2. One timeline value per submit, checked from the completion counter: it is monotonic, it
    // never claims more values than were submitted, and the only values it has not reached are the
    // frames still in flight - at most the two slots the swapchain paces ("strictly one per submit
    // while frames_in_flight <= 2").
    ok = expect(timeline_monotonic, "the timeline value never moved backwards") && ok;
    ok = expect(timeline_never_ahead, "the timeline value never ran ahead of the submits") && ok;
    ok = expect(timeline_end > 0, "the timeline advanced at least once") && ok;
    ok = expect(frames - timeline_end <= kFramesInFlight,
                "at most two submitted values were still in flight") &&
         ok;
    ok =
        expect(max_in_flight >= 1, "the timeline really gated frames (a slot was in flight)") && ok;
    ok = expect(max_in_flight <= kFramesInFlight, "no more than two frames were ever in flight") &&
         ok;

    // 3. Validation-clean: teardown already ran (the host scope closed above), so this covers
    // device creation, the multi-frame run and the bounded drain in one assertion.
    const std::uint64_t messages = tessera::rendering::vulkan::validation_message_count();
    ok = expect(messages == 0, "the run produced no validation messages") && ok;

    if (!ok) {
        std::cerr << "vulkan_timeline_frames: failed (presented=" << presented
                  << " frames=" << frames << " timeline=" << timeline_end
                  << " max_in_flight=" << max_in_flight << " slot_skips=" << slots_skipped
                  << " validation=" << messages << ")\n";
        return 1;
    }
    std::cout << "vulkan_timeline_frames: ok (presented=" << presented << " frames=" << frames
              << " timeline=" << timeline_end << " max_in_flight=" << max_in_flight
              << " slot_skips=" << slots_skipped << " validation=" << messages << ")\n";
    return 0;
}
