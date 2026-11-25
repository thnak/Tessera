// SPDX-License-Identifier: MIT
//
// The host half of ADR-074's fold media features: `@media (device-posture)` and the
// viewport-segment features answer from the fold in the window metrics the host is given, the
// cascade re-resolves only when one of those answers moves, and the hinge angle and rectangle never
// move it. Headless: ManualClock, no GPU, no window.

#include "tessera/app/host.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace tessera;
using tessera::app::AppHost;
using tessera::app::HostConfig;
using tessera::ui::Node;
using tessera::ui::style::MediaPosture;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_host_fold_media: " << message << "\n";
        ++failures;
    }
    return condition;
}

// One rule per fold feature, each setting a different property, so the cascade's decision for each
// is readable from the computed style rather than from the context fields alone.
constexpr std::string_view kCss = R"(
.root { background-color: #000000; color: #aaaaaa; }
@media (device-posture: folded) { .root { background-color: #111111; } }
@media (horizontal-viewport-segments: 2) { .root { color: #222222; } }
)";

std::string property(AppHost& host, const Node& node, const char* name) {
    const ui::style::ComputedStyle style = host.styles().compute(node);
    const std::string* const value = style.find(name);
    return value == nullptr ? std::string{} : *value;
}

hal::WindowMetrics metrics_with(hal::FoldPosture posture, float width = 1000.0f,
                                float height = 700.0f) {
    hal::WindowMetrics metrics;
    metrics.logical_width = static_cast<std::uint32_t>(width);
    metrics.logical_height = static_cast<std::uint32_t>(height);
    metrics.dpi_scale = 1.0f;
    metrics.fold = hal::fold_info_for(posture, metrics.logical_width, metrics.logical_height);
    return metrics;
}

void frame(AppHost& host, double& now) {
    now += 1.0 / 60.0;
    (void)host.begin_frame(now);
    host.end_frame();
}

} // namespace

int main() {
    runtime::ManualClock clock;
    runtime::RuntimeLoop loop(clock);
    Node root(1);
    root.set_class_name("root");
    const ui::style::PreparedStyleSheet prepared = ui::style::prepare_stylesheet(kCss);
    HostConfig config;
    config.width = 1000.0f;
    config.height = 700.0f;
    AppHost host(root, prepared, loop, config);
    double now = 0.0;
    frame(host, now);

    // --- 1. No fold: continuous, one segment, the base rules ---------------------------------
    expect(host.media_context().device_posture == MediaPosture::Continuous,
           "a host with no fold is continuous");
    expect(property(host, root, "background-color") == "#000000", "the base background applies");
    expect(property(host, root, "color") == "#aaaaaa", "the base color applies");

    // --- 2. A book fold: folded, two horizontal segments, both rules apply -------------------
    const std::uint64_t before_fold = host.cascade_generation();
    host.set_window_metrics(metrics_with(hal::FoldPosture::Book));
    frame(host, now);
    expect(host.media_context().device_posture == MediaPosture::Folded, "a book fold is folded");
    expect(host.media_context().horizontal_viewport_segments == 2 &&
               host.media_context().vertical_viewport_segments == 1,
           "a vertical hinge is two horizontal segments");
    expect(property(host, root, "background-color") == "#111111",
           "@media (device-posture: folded) applies");
    expect(property(host, root, "color") == "#222222",
           "@media (horizontal-viewport-segments: 2) applies");
    expect(host.cascade_generation() == before_fold + 1, "the fold change re-resolved once");

    // --- 3. The hinge angle and rectangle are not media answers ------------------------------
    // A slow fold sends a snapshot per degree. None of them may touch the cascade.
    const std::uint64_t settled = host.cascade_generation();
    hal::WindowMetrics sweep = metrics_with(hal::FoldPosture::Book);
    for (float angle = 95.0f; angle <= 100.0f; angle += 1.0f) {
        sweep.fold.hinge_angle_degrees = angle;
        sweep.fold.hinge_bounds.x += 1.0f;
        host.set_window_metrics(sweep);
        frame(host, now);
    }
    expect(host.cascade_generation() == settled,
           "a changing hinge angle and rectangle re-resolve nothing");
    expect(property(host, root, "background-color") == "#111111",
           "and the answers stay as they were");

    // --- 4. A resize keeps the fold answers --------------------------------------------------
    // The context is rebuilt from the rectangle on a resize; the fold half must survive that.
    host.resize(900.0f, 700.0f, 1.0f);
    frame(host, now);
    expect(host.media_context().device_posture == MediaPosture::Folded &&
               host.media_context().horizontal_viewport_segments == 2,
           "a resize does not reset the fold answers");

    // --- 5. Tabletop: still folded, the axis moves ------------------------------------------
    host.set_window_metrics(metrics_with(hal::FoldPosture::Tabletop, 900.0f, 700.0f));
    frame(host, now);
    expect(host.media_context().device_posture == MediaPosture::Folded &&
               host.media_context().horizontal_viewport_segments == 1 &&
               host.media_context().vertical_viewport_segments == 2,
           "a horizontal hinge is two vertical segments");
    expect(property(host, root, "background-color") == "#111111", "still folded");
    expect(property(host, root, "color") == "#aaaaaa",
           "the horizontal-segments rule no longer applies");

    // --- 6. Unfolding restores the base rules -----------------------------------------------
    host.set_window_metrics(metrics_with(hal::FoldPosture::Flat, 900.0f, 700.0f));
    frame(host, now);
    expect(host.media_context().device_posture == MediaPosture::Continuous &&
               host.media_context().horizontal_viewport_segments == 1 &&
               host.media_context().vertical_viewport_segments == 1,
           "a flat fold is continuous with one segment each way");
    expect(property(host, root, "background-color") == "#000000",
           "unfolding restores the base background");

    if (failures == 0) {
        std::cout << "app_host_fold_media: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
