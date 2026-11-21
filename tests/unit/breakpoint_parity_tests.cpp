// SPDX-License-Identifier: MIT
//
// `useBreakpoint()` preview parity (docs/specs/jsx-subset-v1.md section 7.1, issue #692): the
// Studio canvas session and `AppHost` give the same class for the same logical width, because both
// compute it from the same `MediaContext` through the same function. The canvas feeds it from
// `set_viewport`, the host from `resize`/`set_window_metrics`.

#include "scene_canvas_session.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/responsive/environment_set.hpp"
#include "tessera/ui/responsive/size_class.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace ui = tessera::ui;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "breakpoint_parity: " << message << "\n";
    }
    return condition;
}

// A component that renders its own class, so the preview's tree carries the value the hook
// returned.
constexpr std::string_view kBreakpoints = R"(export function Breakpoints(): Node {
  const bp = useBreakpoint();
  return (
    <View>
      <Text>{bp}</Text>
    </View>
  );
}
)";

// The widths the issue names, plus one inside each class.
constexpr float kWidths[] = {320.0f, 599.0f, 600.0f, 700.0f, 839.0f, 840.0f, 1200.0f};

int the_canvas_and_the_host_agree() {
    for (const float width : kWidths) {
        // The canvas: one session, one viewport change.
        cli::CanvasSceneSession canvas;
        cli::CanvasSessionSource source;
        source.text = kBreakpoints;
        source.file = "ui/Breakpoints.tsx";
        source.salt = "preview";
        source.width = width;
        source.height = 240.0f;
        source.dpi_scale = 1.0f;
        const std::string error = canvas.start(source);
        if (!expect(error.empty(), "the canvas evaluates: " + error)) {
            return 1;
        }

        // The host: one window, one resize.
        ui::Node root(1);
        auto sheet = ui::style::compile_stylesheet("* { width: 100%; }");
        tessera::runtime::ManualClock clock;
        tessera::runtime::RuntimeLoop loop(clock);
        tessera::app::HostConfig config;
        config.width = width;
        config.height = 240.0f;
        tessera::app::AppHost host(root, std::move(sheet), loop, config);

        if (!expect(canvas.size_class() == host.size_class(),
                    "the canvas and the host agree at " + std::to_string(width) + " px")) {
            return 2;
        }
    }
    return 0;
}

int the_canvas_pushes_on_a_crossing() {
    cli::CanvasSceneSession canvas;
    cli::CanvasSessionSource source;
    source.text = kBreakpoints;
    source.file = "ui/Breakpoints.tsx";
    source.salt = "preview";
    source.width = 320.0f;
    source.height = 240.0f;
    source.dpi_scale = 1.0f;
    const std::string error = canvas.start(source);
    if (!expect(error.empty(), "the canvas evaluates: " + error)) {
        return 3;
    }
    if (!expect(canvas.size_class() == ui::responsive::SizeClass::Compact, "320 is compact")) {
        return 4;
    }
    // A viewport change across 600 moves the class in the same call, exactly as `AppHost` does.
    if (!expect(canvas.set_viewport(700.0f, 240.0f, 1.0f), "the viewport change lays out")) {
        return 5;
    }
    if (!expect(canvas.size_class() == ui::responsive::SizeClass::Medium, "700 is medium")) {
        return 6;
    }
    // A resize inside the class still lays out (the viewport moved), but the class does not move.
    if (!expect(canvas.set_viewport(800.0f, 240.0f, 1.0f), "a resize inside the class lays out")) {
        return 7;
    }
    if (!expect(canvas.size_class() == ui::responsive::SizeClass::Medium, "800 is still medium")) {
        return 8;
    }
    return 0;
}

} // namespace

int main() {
    if (const int rc = the_canvas_and_the_host_agree(); rc != 0) {
        return rc;
    }
    if (const int rc = the_canvas_pushes_on_a_crossing(); rc != 0) {
        return rc;
    }
    std::cout << "breakpoint_parity: ok\n";
    return 0;
}
