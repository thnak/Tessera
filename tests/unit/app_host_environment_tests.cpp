// SPDX-License-Identifier: MIT
//
// The host half of ADR-074's `env()`: the standard variables are built from the window metrics the
// host is given, a sheet's `env()` follows them, and the cascade re-resolves only when a variable
// the sheet reads has a new value. Headless: ManualClock, no GPU, no window.

#include "tessera/app/host.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/scene/node.hpp"
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

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_host_environment: " << message << "\n";
        ++failures;
    }
    return condition;
}

// The top inset and the second segment's width are the two variables read; the bottom inset is
// deliberately not read, so a change to it must cost nothing. The media rule forces a cascade
// rebuild on a resize, to prove the environment survives one.
constexpr std::string_view kCss = R"(
.root {
    padding-top: env(safe-area-inset-top, 1px);
    margin-left: env(viewport-segment-width 1 0, 7px);
    color: #aaaaaa;
}
@media (max-width: 500px) { .root { color: #222222; } }
)";

std::string property(AppHost& host, const Node& node, const char* name) {
    const ui::style::ComputedStyle style = host.styles().compute(node);
    const std::string* const value = style.find(name);
    return value == nullptr ? std::string{} : *value;
}

hal::WindowMetrics metrics(float top, float bottom, hal::FoldPosture posture) {
    hal::WindowMetrics result;
    result.logical_width = 1000;
    result.logical_height = 700;
    result.dpi_scale = 1.0f;
    result.insets.top = top;
    result.insets.bottom = bottom;
    result.fold = hal::fold_info_for(posture, result.logical_width, result.logical_height);
    return result;
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

    // 1. Before any metrics there is no environment: every env() takes its fallback.
    expect(property(host, root, "padding-top") == "1px", "no metrics: the fallback applies");
    expect(property(host, root, "margin-left") == "7px", "no metrics: the segment fallback");

    // 2. The first snapshot gives the sheet its values, and re-resolves once.
    std::uint64_t generation = host.cascade_generation();
    host.set_window_metrics(metrics(30.0f, 0.0f, hal::FoldPosture::None));
    frame(host, now);
    expect(property(host, root, "padding-top") == "30px", "the inset reaches the sheet");
    expect(property(host, root, "margin-left") == "7px",
           "no fold: a second segment does not exist, so its fallback still applies");
    expect(host.cascade_generation() == generation + 1, "the first snapshot re-resolved once");

    // 3. A variable the sheet does not read costs nothing; neither does an identical snapshot.
    generation = host.cascade_generation();
    host.set_window_metrics(metrics(30.0f, 34.0f, hal::FoldPosture::None));
    host.set_window_metrics(metrics(30.0f, 34.0f, hal::FoldPosture::None));
    frame(host, now);
    expect(host.cascade_generation() == generation, "an unread variable re-resolves nothing");

    // 4. A variable the sheet reads re-resolves it.
    host.set_window_metrics(metrics(44.0f, 34.0f, hal::FoldPosture::None));
    frame(host, now);
    expect(property(host, root, "padding-top") == "44px", "a changed inset follows");
    expect(host.cascade_generation() == generation + 1, "and re-resolves once");

    // 5. A fold splits the viewport: the second segment exists and has the hinge's far side.
    const hal::FoldInfo fold = hal::fold_info_for(hal::FoldPosture::Book, 1000, 700);
    const float far_side = 1000.0f - (fold.hinge_bounds.x + fold.hinge_bounds.width);
    host.set_window_metrics(metrics(44.0f, 34.0f, hal::FoldPosture::Book));
    frame(host, now);
    const std::string expected = std::to_string(static_cast<int>(far_side)) + "px";
    expect(property(host, root, "margin-left") == expected,
           "a book fold gives the second segment its width");

    // 6. A rebuild of the cascade for another reason keeps the environment.
    host.resize(400.0f, 700.0f, 1.0f);
    frame(host, now);
    expect(property(host, root, "color") == "#222222", "the media rule applied (a rebuild ran)");
    expect(property(host, root, "padding-top") == "44px", "the rebuild kept the environment");

    if (failures == 0) {
        std::cout << "app_host_environment: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
