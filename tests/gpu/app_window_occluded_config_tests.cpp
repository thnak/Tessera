// SPDX-License-Identifier: MIT
//
// Runtime-config pause wiring on the real window path (docs/specs/runtime-config-v1.md
// section 6.2): `WindowOptions::runtime.pauseWhenOccluded` decides whether the occluded/minimized
// pause machinery runs. Two sequential hidden-window hosts drive the same lifecycle snapshot
// through WindowHost::pump_once():
//   - the default (`pauseWhenOccluded=true`) presents nothing while occluded, even with a dirty
//     scene, and presents again on resume;
//   - `pauseWhenOccluded=false` keeps presenting while the window is reported occluded and
//     minimized, with the pause counter untouched (focus/blur behaviour is not part of the key).
// Skips with a printed reason when there is no window backend or presentation surface.

#include "tessera/app/host.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
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
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

enum class ArmResult { Skip, Pass, Fail };

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_occluded_config: " << message << "\n";
    }
    return condition;
}

// A complete snapshot at the host's current size: apply_metrics takes the whole lifecycle state, so
// a partial one would exercise the resize path instead of the pause.
WindowMetrics snapshot(WindowHost& host, bool focused, bool occluded, bool minimized) {
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
    metrics.minimized = minimized;
    return metrics;
}

void set_label(Node& label, const char* text) {
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = text;
    label.set_paint_data(std::move(data));
}

// One presenting iteration, retried across a bounded wait.
//
// `render_and_present()` skips a frame whose target slot still has GPU work in flight instead of
// waiting on the GPU, so a single `pump_once()` can return false for a busy slot; `run()` retries
// the same way after latching a full repaint. Retrying here keeps the arms about the
// pauseWhenOccluded policy rather than GPU slot recycling (issue #319).
bool pump_until_presented(WindowHost& host) {
    constexpr int kMaxAttempts = 200;
    constexpr auto kRetryPause = std::chrono::milliseconds(2);
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (host.pump_once()) {
            return true;
        }
        std::this_thread::sleep_for(kRetryPause);
    }
    return false;
}

class Scene final {
public:
    Node root{1};
    Node& label;

    Scene()
        : label(root.add_child(std::make_unique<Node>(2))) {
        label.set_element_kind("Text");
        set_label(label, "Occluded config frame");
    }

    // A scene change that would paint if the driver got to run: the dirty flags are the frame
    // cause, so "no presents while paused" is not just "nothing wanted a frame".
    void touch(const char* text) {
        set_label(label, text);
        label.mark_dirty(Dirty::Paint | Dirty::Render);
    }
};

// One hidden-window host driven by the deterministic single-iteration seam. `pause_when_occluded`
// comes from the runtime config; the scene is dirtied after the occluded snapshot.
ArmResult run_arm(bool pause_when_occluded, const char* arm_name) {
    Scene scene;
    tessera::app::WindowOptions options;
    options.title = "Tessera occluded config";
    options.width = 320;
    options.height = 200;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    options.runtime.pause_when_occluded = pause_when_occluded;

    WindowHost host(scene.root, compile_stylesheet("Text { font-size: 16px; }"), options);
    if (!host.available()) {
        std::cout << "app_window_occluded_config: skipped (no window backend or presentation "
                     "surface)\n";
        return ArmResult::Skip;
    }
    const WindowFrameStats& stats = host.stats();

    if (!expect(pump_until_presented(host), "a fresh host presents on its first iteration")) {
        return ArmResult::Fail;
    }

    const std::uint32_t presented_before = stats.presented;
    const std::uint32_t frames_before = stats.frames;
    const std::uint32_t paused_before = stats.paused_iterations;
    host.apply_metrics(snapshot(host, true, true, false));
    scene.touch("Occluded dirty frame");

    if (pause_when_occluded) {
        if (!expect(!host.pump_once(), "pauseWhenOccluded=true presents nothing while occluded") ||
            !expect(stats.presented == presented_before,
                    "the occluded iteration presented no frame") ||
            !expect(stats.frames == frames_before,
                    "the occluded iteration painted no frame despite the dirty scene") ||
            !expect(stats.paused_iterations == paused_before + 1,
                    "the occluded iteration is counted as paused")) {
            return ArmResult::Fail;
        }
        // The pause, not a lost frame cause, is what skipped the frame: the resume paints.
        host.apply_metrics(snapshot(host, true, false, false));
        if (!expect(pump_until_presented(host), "clearing occlusion presents again")) {
            return ArmResult::Fail;
        }
        if (!expect(stats.presented == presented_before + 1, "the resume presented one frame")) {
            return ArmResult::Fail;
        }
    } else {
        if (!expect(pump_until_presented(host),
                    "pauseWhenOccluded=false keeps painting while occluded") ||
            !expect(stats.presented == presented_before + 1,
                    "the occluded iteration presented one frame") ||
            !expect(stats.frames == frames_before + 1,
                    "the occluded iteration painted one frame") ||
            !expect(stats.paused_iterations == paused_before,
                    "the disabled policy never counts a paused iteration")) {
            return ArmResult::Fail;
        }
        // Minimized follows the same key: still no pause.
        const std::uint32_t presented_after_occluded = stats.presented;
        host.apply_metrics(snapshot(host, true, false, true));
        scene.touch("Minimized dirty frame");
        if (!expect(pump_until_presented(host),
                    "pauseWhenOccluded=false keeps painting while minimized") ||
            !expect(stats.presented == presented_after_occluded + 1,
                    "the minimized iteration presented one frame") ||
            !expect(stats.paused_iterations == paused_before,
                    "minimized never counts a paused iteration when disabled")) {
            return ArmResult::Fail;
        }
        // Focus/blur is not part of the key: an unfocused but visible window paints either way.
        host.apply_metrics(snapshot(host, false, false, false));
        scene.touch("Unfocused dirty frame");
        if (!expect(pump_until_presented(host), "a visible unfocused window still paints") ||
            !expect(stats.paused_iterations == paused_before, "blur never pauses")) {
            return ArmResult::Fail;
        }
    }

    std::cout << "app_window_occluded_config: " << arm_name << " ok (frames=" << stats.frames
              << " presented=" << stats.presented
              << " paused_iterations=" << stats.paused_iterations << ")\n";
    return ArmResult::Pass;
}

} // namespace

int main() {
    const ArmResult paused = run_arm(true, "pause arm");
    if (paused == ArmResult::Skip) {
        return 0;
    }
    if (paused == ArmResult::Fail) {
        return 1;
    }
    const ArmResult keeps_rendering = run_arm(false, "keep-rendering arm");
    if (keeps_rendering == ArmResult::Skip) {
        // A second window host in one process can be refused by a driver; the disabled-policy arm
        // is then covered at the unit level by window_config_tests. Say so instead of passing
        // silently.
        std::cout << "app_window_occluded_config: skipped the keep-rendering arm (a second window "
                     "host is unavailable in this process)\n";
        return 0;
    }
    if (keeps_rendering == ArmResult::Fail) {
        return 2;
    }
    std::cout << "app_window_occluded_config: ok\n";
    return 0;
}
