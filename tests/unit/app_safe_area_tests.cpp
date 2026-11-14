// SPDX-License-Identifier: MIT
//
// SafeArea in the app host (issue #223, docs/specs/safe-area-v1.md sections 4 and 6): a node with a
// safe-area spec gets its padding or margin from the window metrics before layout, the keyboard
// inset eases per edge under the manual clock, reduced motion jumps, and a static window with
// unchanged insets does no layout work.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <iostream>
#include <memory>

namespace {

using tessera::ui::Node;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_safe_area: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.5f;
}

constexpr double kFrame = 1.0 / 60.0;

tessera::hal::WindowMetrics phone() {
    tessera::hal::WindowMetrics metrics;
    metrics.logical_width = 400;
    metrics.logical_height = 800;
    return metrics;
}

// One window: root -> safe (the SafeArea node) -> content (fills what is left).
struct Fixture {
    explicit Fixture(bool reduced_motion = false, bool margin = false)
        : root(1), loop(clock) {
        safe = &root.add_child(std::make_unique<Node>(2));
        safe->set_element_kind("View");
        safe->set_class_name("fill");
        content = &safe->add_child(std::make_unique<Node>(3));
        content->set_element_kind("View");
        content->set_class_name("fill");
        safe->set_inline_style("--safe-area", "on");
        if (margin) {
            safe->set_inline_style("--safe-mode", "margin");
        }
        tessera::app::HostConfig config;
        config.width = 400.0f;
        config.height = 800.0f;
        config.reduced_motion = reduced_motion;
        host = std::make_unique<tessera::app::AppHost>(
            root,
            tessera::ui::style::compile_stylesheet(
                ".fill { flex-grow: 1; display: flex; flex-direction: column; }"),
            loop, config);
    }

    void frame() {
        (void)host->begin_frame(kFrame);
        host->end_frame();
    }

    tessera::runtime::ManualClock clock;
    Node root;
    Node* safe = nullptr;
    Node* content = nullptr;
    tessera::runtime::RuntimeLoop loop;
    std::unique_ptr<tessera::app::AppHost> host;
};

} // namespace

int main() {
    // Notched phone: the first metrics apply at once.
    {
        Fixture f;
        auto metrics = phone();
        metrics.insets = {0, 24, 0, 0};
        f.host->set_window_metrics(metrics);
        f.frame();
        expect(near(f.content->world_bounds().y, 24.0f), "the content starts below the status bar");
        expect(near(f.content->world_bounds().height, 776.0f), "the content fills the rest");
        expect(near(f.safe->world_bounds().y, 0.0f) && near(f.safe->world_bounds().height, 800.0f),
               "padding mode keeps the SafeArea node itself edge to edge");

        // Static window, unchanged insets: no layout work, nothing due.
        const std::size_t runs = f.host->layout_runs();
        f.host->set_window_metrics(metrics);
        const auto idle = f.host->begin_frame(kFrame);
        f.host->end_frame();
        expect(!idle.tick.needs_layout && f.host->layout_runs() == runs,
               "unchanged insets cause no layout");
    }

    // Keyboard: the bottom inset eases up while the top edge stays put.
    {
        Fixture f;
        auto metrics = phone();
        metrics.insets = {0, 24, 0, 0};
        f.host->set_window_metrics(metrics);
        f.frame();
        metrics.ime_insets = {0, 0, 0, 300};
        f.host->set_window_metrics(metrics);
        f.frame();
        const float after_one = f.content->world_bounds().height;
        expect(after_one < 776.0f && after_one > 476.0f, "one frame is part of the way");
        expect(near(f.content->world_bounds().y, 24.0f), "the top edge does not move");
        float previous = after_one;
        bool monotonic = true;
        for (int i = 0; i < 90; ++i) {
            f.frame();
            const float height = f.content->world_bounds().height;
            monotonic = monotonic && height <= previous + 0.01f;
            previous = height;
        }
        expect(monotonic, "the keyboard inset only grows while easing");
        expect(near(f.content->world_bounds().height, 476.0f), "it settles on the keyboard height");
        const std::size_t runs = f.host->layout_runs();
        f.frame();
        expect(f.host->layout_runs() == runs, "a settled animation costs no layout");
    }

    // Reduced motion applies the final value at once.
    {
        Fixture f(true);
        auto metrics = phone();
        f.host->set_window_metrics(metrics);
        f.frame();
        metrics.ime_insets = {0, 0, 0, 300};
        f.host->set_window_metrics(metrics);
        f.frame();
        expect(near(f.content->world_bounds().height, 500.0f), "reduced motion jumps to the end");
    }

    // Margin mode leaves the node's own background out of the unsafe area.
    {
        Fixture f(false, true);
        auto metrics = phone();
        metrics.insets = {0, 24, 0, 0};
        f.host->set_window_metrics(metrics);
        f.frame();
        expect(near(f.safe->world_bounds().y, 24.0f) && near(f.safe->world_bounds().height, 776.0f),
               "margin mode shrinks the SafeArea node");
    }

    // Right-to-left start/end and a book-mode hinge resolve through the same arithmetic.
    {
        Fixture f;
        f.safe->set_inline_style("--safe-edges", "start");
        f.safe->set_inline_style("direction", "rtl");
        auto metrics = phone();
        metrics.insets = {30, 0, 6, 0};
        f.host->set_window_metrics(metrics);
        f.frame();
        expect(near(f.content->world_bounds().x, 0.0f) && near(f.content->world_bounds().width, 394.0f),
               "start is the right edge in right-to-left");
    }
    {
        Fixture f;
        auto metrics = phone();
        metrics.fold.orientation = tessera::hal::FoldOrientation::Vertical;
        metrics.fold.occlusion = tessera::hal::FoldOcclusion::Full;
        metrics.fold.hinge_bounds = {180, 0, 40, 800};
        f.host->set_window_metrics(metrics);
        f.frame();
        expect(near(f.content->world_bounds().width, 180.0f), "a single pane keeps the left half");
    }

    return failures == 0 ? 0 : 1;
}
