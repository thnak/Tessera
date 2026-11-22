// SPDX-License-Identifier: MIT
//
// The caret blink reaching the paint payload: `WindowOptions::caret_active` is the app's policy and
// `WindowOptions::caret_target` is where the driver pushes the blink result. Without both, the
// host's blink state machine runs but nothing it decides ever reaches `TextDecoration`, so paint
// draws a solid caret forever - which is exactly the defect this pins.
//
// Deterministic: a ManualClock, a hidden window, no sleeps beyond the bounded present retry, and no
// real OS focus change. Skips when the platform has no window backend or presentation surface.

#include "tessera/app/host.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/text/edit_controller.hpp"
#include "tessera/ui/text/text_input_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::app::AppHost;
using tessera::app::WindowFrameStats;
using tessera::app::WindowHost;
using tessera::hal::WindowMetrics;
using tessera::runtime::ManualClock;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::text::EditController;
using tessera::ui::text::TextInputRuntime;

// One iteration that must present. A real GPU with vsync may have no free swapchain image at the
// instant of the call (a slot is skipped until its submission completes; the loop never waits on a
// fence), so the frame is late rather than lost. Retry for a bounded time.
bool pump_present(WindowHost& host) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (host.pump_once()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

constexpr double kHalfPeriod = 0.5001;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_caret_target: " << message << "\n";
    }
    return condition;
}

// The app's policy, standing in for the shell's focus check. A plain bool behind a context pointer
// is enough: the driver only asks, it never writes.
struct Policy {
    bool focused = false;
};

bool policy_active(void* user) {
    return static_cast<Policy*>(user)->focused;
}

WindowMetrics snapshot(WindowHost& host, bool focused, bool occluded) {
    const AppHost& current = host.host();
    WindowMetrics metrics;
    metrics.logical_width = static_cast<std::uint32_t>(current.width());
    metrics.logical_height = static_cast<std::uint32_t>(current.height());
    const auto extent = current.target_extent();
    metrics.physical_width = extent.width;
    metrics.physical_height = extent.height;
    metrics.dpi_scale = current.dpi_scale();
    metrics.focused = focused;
    metrics.occluded = occluded;
    metrics.minimized = false;
    return metrics;
}

// The decoration the runtime last published into the field's paint data.
const tessera::ui::TextDecoration* published(Node& field) {
    const std::optional<PaintData>& data = field.paint_data();
    return data.has_value() && data->decoration.has_value() ? &*data->decoration : nullptr;
}

} // namespace

int main() {
    Node root(1);
    auto field_ptr = std::make_unique<Node>(2);
    Node& field = root.add_child(std::move(field_ptr));
    field.set_element_kind("Text");
    {
        PaintData data;
        data.kind = tessera::ui::PaintKind::Text;
        data.text = "";
        field.set_paint_data(std::move(data));
    }

    EditController editor;
    TextInputRuntime runtime(editor);
    runtime.attach(&field);

    Policy policy;
    ManualClock clock;
    tessera::app::WindowOptions options;
    options.title = "Tessera caret target";
    options.width = 320;
    options.height = 200;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    options.clock = &clock;
    options.caret_active = &policy_active;
    options.caret_active_user = &policy;
    options.caret_target = &runtime;

    WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"), options);
    if (!host.available()) {
        std::cout
            << "app_window_caret_target: skipped (no window backend or presentation surface)\n";
        return 0;
    }
    const WindowFrameStats& stats = host.stats();

    // 0. Unfocused: the policy says no, so the caret is inactive and the published phase hides it.
    //    A fresh host must not paint a caret for a field nobody is editing.
    if (!expect(!host.caret_visible(), "an unfocused field has no visible caret") ||
        !expect(pump_present(host), "the startup iteration presents")) {
        return 1;
    }
    const tessera::ui::TextDecoration* decoration = published(field);
    if (!expect(decoration != nullptr, "the runtime published a decoration")) {
        return 1;
    }
    if (!expect(decoration->blink_phase < 0.5F, "an inactive caret publishes a hidden phase")) {
        return 1;
    }

    // 1. Focus arrives through the policy alone - the app never calls set_caret_active - and the
    //    very next iteration shows the caret and publishes a visible phase.
    policy.focused = true;
    if (!expect(pump_present(host), "the focus iteration presents")) {
        return 1;
    }
    if (!expect(host.caret_visible(), "the policy activated the caret") ||
        !expect(stats.caret_blinks == 0, "a policy change is not a blink flip")) {
        return 1;
    }
    decoration = published(field);
    if (!expect(decoration != nullptr && decoration->blink_phase >= 0.5F,
                "a focused caret publishes a visible phase")) {
        return 1;
    }

    // 2. The first half-period flips the blink, and the flip reaches the payload: this is the
    //    assertion that fails when the driver ticks but never publishes.
    clock.advance(kHalfPeriod);
    if (!expect(pump_present(host), "the visible -> hidden flip presents")) {
        return 1;
    }
    if (!expect(!host.caret_visible(), "the caret is hidden after the half-period") ||
        !expect(stats.caret_blinks == 1, "exactly one flip was counted")) {
        return 1;
    }
    decoration = published(field);
    if (!expect(decoration != nullptr && decoration->blink_phase < 0.5F,
                "the hidden flip reached the published phase")) {
        return 1;
    }

    // 3. The next half-period flips back, and the payload follows again.
    clock.advance(kHalfPeriod);
    if (!expect(pump_present(host), "the hidden -> visible flip presents")) {
        return 1;
    }
    if (!expect(host.caret_visible(), "the caret is visible again") ||
        !expect(stats.caret_blinks == 2, "the second flip was counted")) {
        return 1;
    }
    decoration = published(field);
    if (!expect(decoration != nullptr && decoration->blink_phase >= 0.5F,
                "the visible flip reached the published phase")) {
        return 1;
    }

    // 4. Losing focus hides the caret immediately and publishes the hidden phase, without waiting
    //    for a half-period: an unfocused field must never paint a caret. The iteration itself
    //    presents nothing - the field that lost focus repaints its own focus state, and the caret
    //    is not a scene node - so the payload is what has to be right, not the frame count.
    policy.focused = false;
    (void)host.pump_once();
    if (!expect(!host.caret_visible(), "blurring hid the caret immediately")) {
        return 1;
    }
    decoration = published(field);
    if (!expect(decoration != nullptr && decoration->blink_phase < 0.5F,
                "blurring published the hidden phase")) {
        return 1;
    }

    // 5. While blurred the caret stays hidden across any number of clock advances: no flip, no
    //    frame, no phase change. This is what keeps a static form idle.
    const std::uint32_t blurred_blinks = stats.caret_blinks;
    const std::uint32_t blurred_presented = stats.presented;
    for (int step = 0; step < 4; ++step) {
        clock.advance(0.5);
        if (!expect(!host.pump_once(), "a blurred caret presents nothing")) {
            return 1;
        }
    }
    if (!expect(stats.caret_blinks == blurred_blinks, "a blurred caret is never ticked") ||
        !expect(stats.presented == blurred_presented, "a blurred caret requested no frame")) {
        return 1;
    }
    decoration = published(field);
    if (!expect(decoration != nullptr && decoration->blink_phase < 0.5F,
                "the phase stayed hidden while blurred")) {
        return 1;
    }

    // 6. A paused window does not tick the caret, so the phase is frozen rather than advanced.
    host.apply_metrics(snapshot(host, true, true));
    policy.focused = true;
    const bool frozen_visible = host.caret_visible();
    clock.advance(kHalfPeriod);
    if (!expect(!host.pump_once(), "a paused window presents nothing") ||
        !expect(host.caret_visible() == frozen_visible, "the caret state is frozen while paused")) {
        return 1;
    }

    std::cout << "app_window_caret_target: ok (presented=" << stats.presented
              << " caret_blinks=" << stats.caret_blinks << ")\n";
    return 0;
}
