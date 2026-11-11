// SPDX-License-Identifier: MIT
//
// Container queries in the settle loop (docs/design/layout-observation.md section 5.4): the
// engine observer marks descendants style-dirty only when a condition is crossed, runs before the
// app observations (so they see the post-style size), and shares the round budget.

#include "tessera/ui/layout/container_query_tracker.hpp"
#include "tessera/ui/layout/layout_observer_settle.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace {

using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::UpdateScheduler;
using tessera::ui::layout::ContainerQueryTracker;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutObservation;
using tessera::ui::layout::LayoutObserverSet;
using tessera::ui::layout::LayoutReadModel;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutSettleState;
using tessera::ui::layout::SettleHooks;
using tessera::ui::layout::SettleStats;
using tessera::ui::layout::SizeChange;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::CompiledStyleSheet;
using tessera::ui::style::SceneStyleTree;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "container_query_settle: " << message << "\n";
    }
    return condition;
}

// root(1) -> box(2, inline-size container) -> child(3). The child is 80 tall when the container
// query matches and 40 tall otherwise, so a style change moves layout.
struct World {
    explicit World(std::uint32_t max_rounds)
        : sheet(
              compile_stylesheet("* { container-type: inline-size }"
                                 "@container (min-width: 600px) { * { background-color: red } }")),
          set(max_rounds) {
        box = &root.add_child(std::make_unique<Node>(2));
        child = &box->add_child(std::make_unique<Node>(3));
        tree = std::make_unique<SceneStyleTree>(root);
        tracker.add(*box, false);
        hooks.relayout = &World::relayout;
        hooks.relayout_context = this;
        hooks.wake = &World::wake;
        hooks.wake_context = this;
        hooks.engine_observe = &World::engine_observe;
        hooks.engine_observe_context = this;
        publish();
        root.clear_dirty(Dirty::All);
        box->clear_dirty(Dirty::All);
        child->clear_dirty(Dirty::All);
    }

    void publish() {
        LayoutResult layout{{LayoutBox{1, {0, 0, 1000, 500}}, LayoutBox{2, {0, 0, width, 200}},
                             LayoutBox{3, {0, 0, width, child_height}}}};
        model.publish(layout, 1.0f);
    }

    // The style pass of the host: re-resolve dirty nodes, then lay out again.
    static void relayout(void* context) {
        auto* world = static_cast<World*>(context);
        if (world->child->dirty() != Dirty::Clean) {
            const auto style = world->tree->compute(
                world->sheet, *world->child, &ContainerQueryTracker::content_size, &world->model);
            const std::string* color = style.find("background-color");
            world->matched = color != nullptr && *color == "red";
            world->child_height = world->matched ? 80.0f : 40.0f;
            world->child->clear_dirty(Dirty::All);
            ++world->restyles;
        }
        world->publish();
        ++world->relayouts;
    }
    static void wake(void* context) {
        ++static_cast<World*>(context)->wakes;
    }
    static bool engine_observe(void* context, const LayoutReadModel& observed) {
        auto* world = static_cast<World*>(context);
        return world->tracker.update(observed, world->sheet.container_queries()) != 0;
    }

    SettleStats tick() {
        return observe_and_settle(set, model, scheduler, hooks, state);
    }

    Node root{1};
    Node* box = nullptr;
    Node* child = nullptr;
    CompiledStyleSheet sheet;
    std::unique_ptr<SceneStyleTree> tree;
    ContainerQueryTracker tracker;
    LayoutReadModel model;
    UpdateScheduler scheduler;
    LayoutObserverSet set;
    LayoutSettleState state;
    SettleHooks hooks;
    float width = 700.0f;
    float child_height = 40.0f;
    bool matched = false;
    int restyles = 0;
    int relayouts = 0;
    int wakes = 0;
};

struct Recorder {
    static void deliver(void* context, const SizeChange& size) {
        auto* self = static_cast<Recorder*>(context);
        ++self->deliveries;
        self->last = size;
    }
    int deliveries = 0;
    SizeChange last;
};

int run() {
    World world(4);
    Recorder recorder;
    LayoutObservation observation(*world.child, &Recorder::deliver, &recorder);
    world.set.attach(observation);

    // First tick: the container was never styled with a size, so its subtree is styled once, and
    // the app observation sees the post-style height in its single delivery.
    SettleStats stats = world.tick();
    if (!expect(stats.engine_rounds == 1 && stats.relayouts == 1, "first tick styles once") ||
        !expect(world.matched && world.restyles == 1, "700 wide matches min-width: 600") ||
        !expect(recorder.deliveries == 1 && recorder.last.height == 80.0f,
                "the app observer sees the post-style size") ||
        !expect(!stats.round_limit_hit && world.wakes == 0, "settled without waking")) {
        return 1;
    }

    // A resize inside the same breakpoint does no style work at all.
    world.width = 800.0f;
    world.publish();
    stats = world.tick();
    if (!expect(stats.engine_rounds == 0 && stats.relayouts == 0 && world.restyles == 1,
                "no restyle inside a breakpoint") ||
        !expect(world.child->dirty() == Dirty::Clean, "descendants stay clean")) {
        return 2;
    }

    // Crossing the breakpoint restyles the subtree in the same tick.
    world.width = 500.0f;
    world.publish();
    stats = world.tick();
    if (!expect(stats.engine_rounds == 1 && world.restyles == 2, "crossing restyles once") ||
        !expect(!world.matched && world.child_height == 40.0f, "narrow container drops the rule") ||
        !expect(recorder.deliveries == 3 && recorder.last.height == 40.0f,
                "the observer got the width change and the final height in one delivery") ||
        !expect(world.tracker.size() == 1, "tracker keeps its container")) {
        return 3;
    }
    return 0;
}

int budget() {
    // One round of budget: the engine round dirties styles, the budget ends the loop, the tick
    // reports the limit and asks for a frame, and the next tick finishes the work.
    World world(1);
    SettleStats stats = world.tick();
    if (!expect(stats.engine_rounds == 1 && stats.round_limit_hit && world.wakes == 1,
                "budget 1: limit hit and woken") ||
        !expect(world.restyles == 0 && world.child->dirty() != Dirty::Clean,
                "budget 1: the styles wait for the next tick")) {
        return 4;
    }
    return 0;
}

} // namespace

int main() {
    if (const int rc = run(); rc != 0) {
        return rc;
    }
    if (const int rc = budget(); rc != 0) {
        return rc;
    }
    std::cout << "container_query_settle: ok\n";
    return 0;
}
