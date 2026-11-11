// SPDX-License-Identifier: MIT
//
// observe_and_settle (docs/design/layout-observation.md sections 5.1, 5.3, 8): a callback that
// grows a child settles in round 2, the depth rule defers a shallower re-dirty, an oscillating
// observation wakes the loop and is reported after kLoopReportTicks, the round budget ends a deep
// chain, max_rounds = 1 is next-tick delivery, and an empty set costs nothing.

#include "tessera/diagnostics/log.hpp"
#include "tessera/ui/layout/layout_observer_settle.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

namespace {

using tessera::ui::Node;
using tessera::ui::UpdateScheduler;
using tessera::ui::UpdateTicket;
using tessera::ui::layout::kLoopReportTicks;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutObservation;
using tessera::ui::layout::LayoutObserverSet;
using tessera::ui::layout::LayoutReadModel;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutSettleState;
using tessera::ui::layout::SettleHooks;
using tessera::ui::layout::SettleStats;
using tessera::ui::layout::SizeChange;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_observer_settle: " << message << "\n";
    }
    return condition;
}

constexpr int kAlways = std::numeric_limits<int>::max();

// root(1) -> a(2) -> b(3) -> c(4): depths 0, 1, 2, 3.
struct World {
    explicit World(std::uint32_t max_rounds)
        : set(max_rounds) {
        a = &root.add_child(std::make_unique<Node>(2));
        b = &a->add_child(std::make_unique<Node>(3));
        c = &b->add_child(std::make_unique<Node>(4));
        ticket.update = &World::run_update;
        ticket.context = this;
        scheduler.attach(ticket);
        hooks.relayout = &World::relayout;
        hooks.relayout_context = this;
        hooks.wake = &World::wake;
        hooks.wake_context = this;
        publish();
    }

    ~World() {
        scheduler.detach(ticket);
    }

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void publish() {
        LayoutResult layout{{LayoutBox{1, {0, 0, 200, 200}}, LayoutBox{2, {0, 0, wa, 50}},
                             LayoutBox{3, {0, 0, wb, 10}}, LayoutBox{4, {0, 0, wc, 5}}}};
        model.publish(layout, 1.0f);
    }

    SettleStats tick() {
        return observe_and_settle(set, model, scheduler, hooks, state);
    }

    static void run_update(void* context) {
        auto* world = static_cast<World*>(context);
        world->update(*world);
    }
    static void relayout(void* context) {
        auto* world = static_cast<World*>(context);
        world->publish();
        ++world->relayouts;
    }
    static void wake(void* context) {
        ++static_cast<World*>(context)->wakes;
    }

    Node root{1};
    Node* a = nullptr;
    Node* b = nullptr;
    Node* c = nullptr;
    float wa = 100.0f;
    float wb = 10.0f;
    float wc = 5.0f;
    LayoutReadModel model;
    UpdateScheduler scheduler;
    UpdateTicket ticket;
    LayoutObserverSet set;
    LayoutSettleState state;
    SettleHooks hooks;
    void (*update)(World&) = nullptr;
    int wakes = 0;
    int relayouts = 0;
};

void grow_b(World& world) {
    world.wb = 50.0f;
}
void grow_a(World& world) {
    world.wa += 1.0f;
}
void grow_all(World& world) {
    world.wa += 1.0f;
    world.wb += 1.0f;
    world.wc += 1.0f;
}

// An observation whose callback schedules the world's update for its first `schedule_until`
// deliveries (kAlways = every delivery).
struct Watcher {
    Watcher(World& owner, Node& node, int until)
        : world(&owner),
          schedule_until(until),
          observation(node, &Watcher::deliver, this) {
        owner.set.attach(observation);
    }

    static void deliver(void* context, const SizeChange& size) {
        auto* self = static_cast<Watcher*>(context);
        ++self->deliveries;
        self->last = size;
        if (self->deliveries <= self->schedule_until) {
            self->world->ticket.schedule();
        }
    }

    World* world = nullptr;
    int schedule_until = 0;
    int deliveries = 0;
    SizeChange last;
    LayoutObservation observation;
};

bool grown_child_settles_in_round_two() {
    World world(4);
    world.update = &grow_b;
    Watcher a(world, *world.a, 1);
    Watcher b(world, *world.b, 0);

    const SettleStats stats = world.tick();
    return expect(stats.rounds == 2, "the callback's change settles in a second round") &&
           expect(stats.deliveries == 3, "a and b initially, then b again with its new size") &&
           expect(stats.relayouts == 1, "one relayout between the rounds") &&
           expect(stats.deferred == 0 && !stats.round_limit_hit, "nothing is left over") &&
           expect(b.deliveries == 2 && b.last.width == 50.0f, "b saw the grown width this tick") &&
           expect(world.wakes == 0, "a settled tick does not wake the loop") &&
           expect(!world.scheduler.pending(), "the scheduler is drained") &&
           expect(world.state.totals().rounds == 2 && world.state.totals().deliveries == 3,
                  "totals accumulate the rounds and deliveries");
}

bool depth_rule_defers_a_shallower_change() {
    World world(4);
    world.update = &grow_a;
    Watcher a(world, *world.a, 0);
    Watcher b(world, *world.b, 1); // b's callback widens its ancestor a

    const SettleStats first = world.tick();
    if (!expect(first.rounds == 1 && first.relayouts == 1, "round 2 finds nothing deep enough") ||
        !expect(first.deferred == 1,
                "a changed but is shallower than the floor, so it is deferred") ||
        !expect(a.deliveries == 1, "a was not delivered in round 2") ||
        !expect(world.wakes == 1, "a deferral wakes the loop")) {
        return false;
    }
    const SettleStats second = world.tick();
    return expect(a.deliveries == 2 && a.last.width == 101.0f,
                  "the next tick delivers the deferred change first") &&
           expect(second.deferred == 0 && world.state.backlog_ticks() == 0,
                  "the backlog is cleared") &&
           expect(world.wakes == 1, "no further wake");
}

bool oscillation_is_reported_once() {
    World world(4);
    world.update = &grow_a;
    Watcher a(world, *world.a, kAlways);

    tessera::diagnostics::MemoryLogSink sink;
    tessera::diagnostics::Logger logger;
    logger.add_sink(sink);
    world.hooks.logger = &logger;

    for (std::uint32_t tick = 1; tick <= kLoopReportTicks + 1; ++tick) {
        const SettleStats stats = world.tick();
        if (!expect(stats.deferred == 1, "every tick defers the oscillating observation") ||
            !expect(stats.loop_reported == (tick == kLoopReportTicks),
                    "the diagnostic fires on the threshold tick only")) {
            return false;
        }
    }
    return expect(static_cast<std::uint32_t>(world.wakes) == kLoopReportTicks + 1,
                  "the loop is woken every tick") &&
           expect(sink.records().size() == 1, "one log record") &&
           expect(sink.records().front().level == tessera::diagnostics::LogLevel::Warn &&
                      sink.records().front().message.rfind("TESSERA_LAYOUT_OBSERVER_LOOP", 0) == 0,
                  "a warning that names TESSERA_LAYOUT_OBSERVER_LOOP") &&
           expect(world.state.totals().loop_reports == 1, "counted once");
}

bool round_budget_ends_a_deep_chain() {
    World world(2);
    world.update = &grow_all;
    Watcher a(world, *world.a, 1);
    Watcher b(world, *world.b, 2);
    Watcher c(world, *world.c, 0);

    const SettleStats stats = world.tick();
    return expect(stats.round_limit_hit, "the budget ended the loop while b still scheduled") &&
           expect(stats.rounds == 2 && stats.relayouts == 1, "two rounds, one relayout") &&
           expect(stats.deliveries == 5, "three initial deliveries plus b and c in round 2") &&
           expect(stats.deferred == 1, "a stays due: it is shallower than the floor") &&
           expect(world.wakes == 1, "the leftovers wake the loop") &&
           expect(world.scheduler.pending(),
                  "the unflushed update stays queued for the next tick") &&
           expect(world.state.totals().round_limit_hits == 1, "the limit hit is counted");
}

bool one_round_is_next_tick_delivery() {
    World world(1);
    world.update = &grow_a;
    Watcher a(world, *world.a, 1);

    const SettleStats first = world.tick();
    if (!expect(first.round_limit_hit && first.relayouts == 0,
                "one round never relayouts inside the tick") ||
        !expect(world.wakes == 1 && world.scheduler.pending(),
                "the pending update wakes the loop for the next tick")) {
        return false;
    }
    // The host's next tick: flush, relayout, then observe.
    world.scheduler.flush();
    world.publish();
    const SettleStats second = world.tick();
    return expect(a.deliveries == 2 && a.last.width == 101.0f,
                  "the new size is delivered on the next tick") &&
           expect(!second.round_limit_hit, "the second tick settles");
}

bool empty_set_costs_nothing() {
    World world(4);
    const SettleStats stats = world.tick();
    return expect(stats.rounds == 0 && stats.deliveries == 0 && stats.relayouts == 0 &&
                      stats.deferred == 0 && !stats.round_limit_hit,
                  "no observation, no work") &&
           expect(world.wakes == 0, "no wake");
}

bool rounds_are_clamped() {
    using tessera::ui::layout::clamp_observation_rounds;
    return expect(clamp_observation_rounds(0) == 1 && clamp_observation_rounds(-3) == 1,
                  "below the range clamps to 1") &&
           expect(clamp_observation_rounds(4) == 4, "in range is kept") &&
           expect(clamp_observation_rounds(99) == 16, "above the range clamps to 16");
}

} // namespace

int main() {
    if (!grown_child_settles_in_round_two()) {
        return 1;
    }
    if (!depth_rule_defers_a_shallower_change()) {
        return 2;
    }
    if (!oscillation_is_reported_once()) {
        return 3;
    }
    if (!round_budget_ends_a_deep_chain()) {
        return 4;
    }
    if (!one_round_is_next_tick_delivery()) {
        return 5;
    }
    if (!empty_set_costs_nothing()) {
        return 6;
    }
    if (!rounds_are_clamped()) {
        return 7;
    }
    std::cout << "layout_observer_settle: ok\n";
    return 0;
}
