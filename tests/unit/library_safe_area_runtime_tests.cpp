// SPDX-License-Identifier: MIT

// The library SafeArea end to end (issue #223, docs/specs/safe-area-v1.md section 9): the generated
// `SafeAreaDemo` and `SafeAreaOptions` pages (golden C++ from packages/tessera-ui/SafeArea.tsx) run
// in the app host with `safe-area.css`, against the scripted devices of the spec: a notched phone,
// a gesture-navigation phone, a tablet with a bottom keyboard and a book-mode foldable.

#include "SafeAreaDemo.hpp"
#include "SafeAreaOptions.hpp"
#include "tessera/app/host.hpp"
#include "tessera/hal/cutout.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

#ifndef TESSERA_SAFE_AREA_CSS
#error "TESSERA_SAFE_AREA_CSS must be defined by the build"
#endif

namespace {

namespace ui = tessera::ui;
namespace hal = tessera::hal;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.5f;
}

// Checks `got` is within half a pixel of `want` and prints both when it is not.
void check_near(float got, float want, const std::string& message) {
    check(std::fabs(got - want) < 0.5f,
          message + " (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

bool has_token(const ui::Node& node, const std::string& token) {
    const std::string classes = " " + std::string(node.class_name()) + " ";
    return classes.find(" " + token + " ") != std::string::npos;
}

ui::Node* find_class(ui::Node& node, const std::string& token) {
    if (has_token(node, token)) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (ui::Node* found = find_class(*child, token)) {
            return found;
        }
    }
    return nullptr;
}

hal::WindowMetrics device(std::uint32_t width, std::uint32_t height) {
    hal::WindowMetrics metrics;
    metrics.logical_width = width;
    metrics.logical_height = height;
    return metrics;
}

// One window around a generated page; `Page` is SafeAreaDemo or SafeAreaOptions.
template<typename Page> struct Window {
    Window(float width, float height)
        : loop(clock) {
        page.build(root);
        std::ifstream css_file(TESSERA_SAFE_AREA_CSS, std::ios::binary);
        const std::string css((std::istreambuf_iterator<char>(css_file)),
                              std::istreambuf_iterator<char>());
        tessera::app::HostConfig config;
        config.width = width;
        config.height = height;
        host = std::make_unique<tessera::app::AppHost>(
            root,
            tessera::ui::style::compile_stylesheet(
                css + "\n.fill { flex-grow: 1; display: flex; flex-direction: column; }"),
            loop, config);
        safe = find_class(root, "tsafe");
        content =
            safe != nullptr && !safe->children().empty() ? safe->children()[0].get() : nullptr;
    }

    void frames(int count) {
        for (int i = 0; i < count; ++i) {
            (void)host->begin_frame(1.0 / 60.0);
            host->end_frame();
        }
    }

    tessera::runtime::ManualClock clock;
    ui::Node root{1000};
    Page page;
    tessera::runtime::RuntimeLoop loop;
    std::unique_ptr<tessera::app::AppHost> host;
    ui::Node* safe = nullptr;
    ui::Node* content = nullptr;
};

} // namespace

int main() {
    // Notched phone: a top-center cutout inside a taller bar; the default SafeArea pads by it.
    {
        Window<safe_area_demo::gen::SafeAreaDemo> w(400, 800);
        check(w.safe != nullptr && w.content != nullptr, "the page has a SafeArea and content");
        if (w.safe == nullptr || w.content == nullptr) {
            return 1;
        }
        hal::WindowMetrics metrics = device(400, 800);
        metrics.insets = {0, 20, 0, 0};
        const std::array<hal::WindowRect, 1> notch{hal::WindowRect{150, 0, 100, 44}};
        metrics.cutouts = hal::make_cutout_info(notch);
        w.host->set_window_metrics(metrics);
        w.frames(1);
        check_near(w.content->world_bounds().y, 44.0f, "content clears the notch");
        check_near(w.content->world_bounds().height, 756.0f, "content fills the rest");
        check_near(w.safe->world_bounds().y, 0.0f, "padding mode keeps the wrapper edge to edge");
    }

    // Gesture-navigation phone: the default ignores the gesture zone; the options page counts it,
    // keeps a floor of 8 on top and bottom only, and leaves the sides edge to edge (margin mode).
    {
        Window<safe_area_demo::gen::SafeAreaDemo> w(400, 800);
        hal::WindowMetrics metrics = device(400, 800);
        metrics.gesture_insets = {24, 0, 24, 20};
        w.host->set_window_metrics(metrics);
        w.frames(1);
        check(w.content != nullptr && near(w.content->world_bounds().height, 800.0f),
              "gestures are off by default");
    }
    {
        Window<safe_area_demo::gen::SafeAreaOptions> w(400, 800);
        check(w.safe != nullptr, "the options page has a SafeArea");
        if (w.safe == nullptr) {
            return 1;
        }
        hal::WindowMetrics metrics = device(400, 800);
        metrics.gesture_insets = {24, 0, 24, 20};
        w.host->set_window_metrics(metrics);
        w.frames(1);
        check_near(w.safe->world_bounds().y, 8.0f, "the minimum floors the top");
        check_near(w.safe->world_bounds().height, 772.0f, "the gesture zone sets the bottom");
        check(near(w.safe->world_bounds().x, 0.0f) && near(w.safe->world_bounds().width, 400.0f),
              "edges top+bottom leave the sides alone");
    }

    // Tablet with a bottom keyboard: the inset eases in and settles on the keyboard height.
    {
        Window<safe_area_demo::gen::SafeAreaDemo> w(800, 600);
        if (w.content == nullptr) {
            return 1;
        }
        hal::WindowMetrics metrics = device(800, 600);
        w.host->set_window_metrics(metrics);
        w.frames(1);
        metrics.ime_insets = {0, 0, 0, 320};
        w.host->set_window_metrics(metrics);
        w.frames(1);
        const float partway = w.content->world_bounds().height;
        check(partway < 600.0f && partway > 280.0f, "the keyboard inset eases in");
        w.frames(120);
        check_near(w.content->world_bounds().height, 280.0f, "it settles on the keyboard height");
    }

    // Book-mode foldable: a full-occlusion vertical hinge; a single pane keeps the larger side.
    {
        Window<safe_area_demo::gen::SafeAreaDemo> w(1000, 700);
        if (w.content == nullptr) {
            return 1;
        }
        hal::WindowMetrics metrics = device(1000, 700);
        metrics.fold.state = hal::FoldState::HalfOpened;
        metrics.fold.orientation = hal::FoldOrientation::Vertical;
        metrics.fold.occlusion = hal::FoldOcclusion::Full;
        metrics.fold.separating = true;
        metrics.fold.hinge_bounds = {300, 0, 40, 700};
        w.host->set_window_metrics(metrics);
        w.frames(1);
        check(near(w.content->world_bounds().x, 340.0f) &&
                  near(w.content->world_bounds().width, 660.0f),
              "content keeps the larger pane beside the hinge");
    }

    if (failures == 0) {
        std::cout << "library safe area runtime passed\n";
    }
    return failures == 0 ? 0 : 1;
}
