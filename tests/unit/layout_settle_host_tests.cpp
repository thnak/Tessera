// SPDX-License-Identifier: MIT
//
// The ADR-041 layout settle loop wired into a real host (issue #693): `@container` rules resolve
// against the published layout, a container that crosses a condition re-styles in the SAME tick as
// the resize that caused it, a resize inside one condition costs one layout pass and no style work,
// and a stylesheet without `@container` pays nothing. Headless: ManualClock, null paint sink, the
// deterministic text stub.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/layout/layout_settle.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_settle_host: " << message << "\n";
    }
    return condition;
}

// Counts the fills so a test can tell "the container rule applied" from "it did not" without a GPU.
class CountingSink final : public tessera::ui::paint::PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        fills.push_back({rect, color});
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float /*radius*/) override {
        fills.push_back({rect, color});
    }
    void draw_text(const TextDraw&) override {}

    struct Fill {
        Rect rect;
        Color color;
    };
    std::vector<Fill> fills;
};

bool has_color(const CountingSink& sink, const Color& wanted) {
    for (const CountingSink::Fill& fill : sink.fills) {
        if (fill.color.r == wanted.r && fill.color.g == wanted.g && fill.color.b == wanted.b) {
            return true;
        }
    }
    return false;
}

constexpr Color kRed{1.0f, 0.0f, 0.0f, 1.0f};
constexpr Color kBlue{0.0f, 0.0f, 1.0f, 1.0f};

// root(1) -> panel(2, the query container) -> child(3). The child is red above 400 px and blue
// below it, so the painted colour is the observable answer to "did the container query apply".
struct World {
    explicit World(std::string css)
        : sheet(compile_stylesheet(css)) {
        panel = &root.add_child(std::make_unique<Node>(2));
        panel->set_element_kind("View");
        panel->set_class_name("panel");
        child = &panel->add_child(std::make_unique<Node>(3));
        child->set_element_kind("View");
        child->set_class_name("child");
    }

    Node root{1};
    Node* panel = nullptr;
    Node* child = nullptr;
    tessera::ui::style::CompiledStyleSheet sheet;
};

// The container query case: the panel is an inline-size container, and the child's background
// flips at 400 px.
constexpr const char* kContainerCss = R"(
.panel { container-type: inline-size; width: 100%; height: 200px; }
.child { width: 50px; height: 50px; background-color: blue; }
@container (min-width: 400px) { .child { background-color: red } }
)";

int container_query_crosses_in_the_same_tick() {
    World world(kContainerCss);
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 320.0f;
    config.height = 240.0f;
    tessera::app::AppHost host(world.root, world.sheet, loop, config);

    // First frame: the container has no published size yet, so the rule is unapplied and the
    // engine observer counts the container as changed, which re-styles it in the same tick.
    const tessera::app::HostFrame first = host.begin_frame(0.0);
    CountingSink first_sink;
    (void)host.paint(first_sink, true);
    if (!expect(first.frame_due, "first frame is due") ||
        !expect(has_color(first_sink, kBlue), "320 px is below the 400 px condition") ||
        !expect(host.layout_settle_totals().engine_rounds >= 1,
                "the first frame settles the container once")) {
        return 1;
    }
    host.end_frame();

    // A resize that crosses the condition: the SAME begin_frame must already paint red, with no
    // frame of stale layout in between.
    host.resize(500.0f, 240.0f, 1.0f);
    const tessera::app::HostFrame wide = host.begin_frame(1.0 / 60.0);
    CountingSink wide_sink;
    (void)host.paint(wide_sink, true);
    if (!expect(wide.frame_due, "the crossing frame is due") ||
        !expect(has_color(wide_sink, kRed), "500 px crosses the condition in the same tick") ||
        !expect(!has_color(wide_sink, kBlue), "the stale colour is not painted")) {
        return 2;
    }
    host.end_frame();

    // A resize inside the same condition: one layout pass, no engine round, no style work.
    const std::uint64_t engine_before = host.layout_settle_totals().engine_rounds;
    host.resize(600.0f, 240.0f, 1.0f);
    const tessera::app::HostFrame inside = host.begin_frame(1.0 / 60.0);
    CountingSink inside_sink;
    (void)host.paint(inside_sink, true);
    if (!expect(has_color(inside_sink, kRed), "600 px still matches") ||
        !expect(host.layout_settle_totals().engine_rounds == engine_before,
                "a resize inside one condition costs no engine round")) {
        return 3;
    }
    host.end_frame();

    // Crossing back down restyles again.
    host.resize(300.0f, 240.0f, 1.0f);
    const tessera::app::HostFrame narrow = host.begin_frame(1.0 / 60.0);
    CountingSink narrow_sink;
    (void)host.paint(narrow_sink, true);
    if (!expect(has_color(narrow_sink, kBlue), "300 px drops the rule again")) {
        return 4;
    }
    host.end_frame();
    return 0;
}

int no_container_queries_costs_nothing() {
    // The same tree without `@container`: the gate is closed, so no read model is published, no
    // tracker runs and no engine round happens.
    World world(R"(
.panel { width: 100%; height: 200px; }
.child { width: 50px; height: 50px; background-color: blue; }
)");
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 320.0f;
    config.height = 240.0f;
    tessera::app::AppHost host(world.root, world.sheet, loop, config);

    (void)host.begin_frame(0.0);
    host.end_frame();
    host.resize(500.0f, 240.0f, 1.0f);
    (void)host.begin_frame(1.0 / 60.0);
    host.end_frame();
    const tessera::ui::layout::SettleTotals& totals = host.layout_settle_totals();
    if (!expect(totals.engine_rounds == 0, "no @container means no engine round") ||
        !expect(totals.rounds == 0, "no @container means no settle round") ||
        !expect(totals.deliveries == 0, "no @container means no delivery")) {
        return 5;
    }
    return 0;
}

int round_budget_terminates_a_self_sizing_rule() {
    // A rule that changes the size of its own container would oscillate. The round budget ends the
    // loop, the tick reports the limit, and the frame still completes: it never hangs.
    World world(R"(
.panel { container-type: inline-size; width: 100%; height: 200px; }
.child { width: 50px; height: 50px; background-color: blue; }
@container (min-width: 100px) { .child { width: 60px; background-color: red } }
)");
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 320.0f;
    config.height = 240.0f;
    // One round: the engine observer's round ends the loop with the styles it dirtied still due.
    config.layout_observation_max_rounds = 1;
    tessera::app::AppHost host(world.root, world.sheet, loop, config);

    const tessera::app::HostFrame frame = host.begin_frame(0.0);
    if (!expect(frame.frame_due, "a budget-limited frame still completes") ||
        !expect(host.layout_settle_totals().round_limit_hits >= 1,
                "the round budget is reported as a limit hit")) {
        return 6;
    }
    host.end_frame();
    // The next tick finishes the work instead of spinning.
    (void)host.begin_frame(1.0 / 60.0);
    host.end_frame();
    return 0;
}

int round_budget_clamps() {
    tessera::ui::layout::LayoutSettle settle;
    settle.set_max_rounds(0);
    if (!expect(settle.max_rounds() == 1, "0 clamps to 1")) {
        return 7;
    }
    settle.set_max_rounds(99);
    if (!expect(settle.max_rounds() == 16, "99 clamps to 16")) {
        return 8;
    }
    settle.set_max_rounds(4);
    if (!expect(settle.max_rounds() == 4, "4 stays 4")) {
        return 9;
    }
    return 0;
}

} // namespace

int main() {
    if (const int rc = container_query_crosses_in_the_same_tick(); rc != 0) {
        return rc;
    }
    if (const int rc = no_container_queries_costs_nothing(); rc != 0) {
        return rc;
    }
    if (const int rc = round_budget_terminates_a_self_sizing_rule(); rc != 0) {
        return rc;
    }
    if (const int rc = round_budget_clamps(); rc != 0) {
        return rc;
    }
    std::cout << "layout_settle_host: ok\n";
    return 0;
}
