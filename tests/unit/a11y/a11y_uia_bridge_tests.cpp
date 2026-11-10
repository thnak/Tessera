// SPDX-License-Identifier: MIT
//
// Bridge thread + snapshot handoff behavior: publication is adopted by client calls (not by the
// idle loop), drop-oldest overflow is counted, and the provider only runs on the bridge thread.

#include "a11y_uia_fake_apartment.hpp"
#include "a11y_uia_test_support.hpp"
#include "tessera/ui/accessibility/uia_bridge.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace a11y_uia_test;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::UiaActionRequest;
using tessera::ui::a11y::UiaBridge;
using tessera::ui::a11y::UiaBridgeConfig;
using tessera::ui::a11y::UiaError;
using tessera::ui::a11y::UiaProvider;

struct Harness {
    FakeApartment* apartment = nullptr;
    std::unique_ptr<UiaBridge> bridge;
};

std::unique_ptr<Harness> make_harness() {
    auto harness = std::make_unique<Harness>();
    auto apartment = std::make_unique<FakeApartment>();
    harness->apartment = apartment.get();
    UiaBridgeConfig config;
    // A long idle wait keeps the worker asleep unless a client job or `step` wakes it, so the
    // snapshot assertions below do not depend on thread scheduling.
    config.idle_wait_ms = 1000;
    config.shutdown_wait_ms = 5000;
    harness->bridge = std::make_unique<UiaBridge>(config, std::move(apartment));
    return harness;
}

bool start_running(Harness& harness, const char* what) {
    UiaBridge& bridge = *harness.bridge;
    if (!expect(bridge.start().has_value(), what)) {
        return false;
    }
    if (!expect(bridge.step().has_value(), "step initializes the apartment")) {
        return false;
    }
    return expect(bridge.running(), "the bridge is running after the first step");
}

bool publication() {
    auto harness = make_harness();
    UiaBridge& bridge = *harness->bridge;
    if (!start_running(*harness, "start succeeds")) {
        return false;
    }

    std::size_t count = 0;
    std::uint64_t generation = 0;
    std::string root_name;
    const auto empty = bridge.run_on_bridge([&](UiaProvider& provider) {
        count = provider.node_count();
        generation = provider.generation();
        root_name.clear();
    });
    if (!expect(empty.has_value(), "an empty provider answers") ||
        !expect_eq(static_cast<std::int64_t>(count), 0, "no nodes before a publish") ||
        !expect_eq(static_cast<std::int64_t>(generation), 0, "generation starts at zero")) {
        return false;
    }

    bridge.publish(button_snapshot("Save"));
    const auto pending = bridge.handoff_stats();
    if (!expect_eq(static_cast<std::int64_t>(pending.published), 1, "publish counted") ||
        !expect_eq(static_cast<std::int64_t>(pending.taken), 0,
                   "the idle loop does not adopt snapshots on its own")) {
        return false;
    }

    bool seen_for_thread = false;
    std::thread::id provider_thread;
    const auto adopted = bridge.run_on_bridge([&](UiaProvider& provider) {
        count = provider.node_count();
        generation = provider.generation();
        seen_for_thread = provider.owns_thread();
        provider_thread = std::this_thread::get_id();
        if (const auto* root = provider.node(provider.root()); root != nullptr) {
            root_name.assign(tessera::ui::a11y::name_of(*root));
        }
        if (const auto* child = provider.node(1); child != nullptr) {
            expect(provider.parent_of(1) == 0, "child parent table");
            expect_eq(provider.index_in_parent(1), 0, "child index in parent");
        }
    });
    const auto drained = bridge.handoff_stats();
    return expect(adopted.has_value(), "a client call adopts the pending snapshot") &&
           expect_eq(static_cast<std::int64_t>(count), 2, "two nodes adopted") &&
           expect_eq(static_cast<std::int64_t>(generation), 1, "generation advanced") &&
           expect_equal(root_name, "Root", "root name") &&
           expect(seen_for_thread, "the provider runs on the bridge thread") &&
           expect(provider_thread == bridge.bridge_thread(), "provider thread identity") &&
           expect_eq(static_cast<std::int64_t>(drained.taken), 1, "one snapshot taken");
}

bool overflow_policy() {
    auto harness = make_harness();
    UiaBridge& bridge = *harness->bridge;
    if (!start_running(*harness, "start succeeds")) {
        return false;
    }
    bridge.publish(button_snapshot("A"));
    bridge.publish(button_snapshot("B"));
    bridge.publish(button_snapshot("C"));
    const auto before = bridge.handoff_stats();
    if (!expect_eq(static_cast<std::int64_t>(before.published), 3, "three publishes") ||
        !expect_eq(static_cast<std::int64_t>(before.dropped), 2,
                   "two replaced publishes dropped")) {
        return false;
    }

    std::string name;
    const auto served = bridge.run_on_bridge([&](UiaProvider& provider) {
        if (const auto* child = provider.node(1); child != nullptr) {
            name.assign(tessera::ui::a11y::name_of(*child));
        }
    });
    const auto after = bridge.handoff_stats();
    return expect(served.has_value(), "the newest snapshot is served") &&
           expect_equal(name, "C", "newest tree wins") &&
           expect_eq(static_cast<std::int64_t>(after.taken), 1, "exactly one take") &&
           expect_eq(static_cast<std::int64_t>(after.dropped), 2, "drop count is stable");
}

bool thread_ownership() {
    auto harness = make_harness();
    UiaBridge& bridge = *harness->bridge;
    if (!start_running(*harness, "start succeeds")) {
        return false;
    }
    bridge.publish(button_snapshot("Save"));
    if (!expect(bridge.step().has_value(), "adopt the snapshot")) {
        return false;
    }

    // A state-changing provider call from the UI thread is rejected before it touches the queue.
    const auto rejected = bridge.provider().request_action(1, Action::Focus);
    if (!expect(!rejected.has_value(), "a UI-thread action is rejected") ||
        !expect(rejected.error() == UiaError::ThreadViolation, "rejection is ThreadViolation") ||
        !expect_eq(static_cast<std::int64_t>(bridge.provider().queued_actions()), 0,
                   "the rejected action queued nothing")) {
        return false;
    }

    bool owns = false;
    bool queued = false;
    const auto result = bridge.run_on_bridge([&](UiaProvider& provider) {
        owns = provider.owns_thread();
        queued = provider.request_action(1, Action::Focus).has_value();
    });
    if (!expect(result.has_value(), "bridge-thread call succeeds") ||
        !expect(owns, "owner thread matches") || !expect(queued, "action queued on bridge")) {
        return false;
    }

    std::vector<UiaActionRequest> actions(4);
    const std::size_t taken = bridge.take_actions(actions);
    return expect_eq(static_cast<std::int64_t>(taken), 1, "one action drained") &&
           expect_eq(static_cast<std::int64_t>(actions[0].node), 1, "drained node") &&
           expect(actions[0].action == Action::Focus, "drained action") &&
           expect_eq(static_cast<std::int64_t>(bridge.take_actions(actions)), 0, "queue empty");
}

bool action_coalescing_and_queue_bound() {
    auto harness = make_harness();
    UiaBridge& bridge = *harness->bridge;
    if (!start_running(*harness, "start succeeds")) {
        return false;
    }

    using namespace tessera::ui::a11y;
    A11yNode root = make_node(Role::Generic, "Root");
    A11yNode field = make_node(Role::TextField, "Email", "a@b");
    field.actions = Action::SetValue | Action::Focus;
    link_children(root, {1});
    bridge.publish(snapshot_of({root, field}));
    if (!expect(bridge.step().has_value(), "adopt the text field snapshot")) {
        return false;
    }
    const auto both = bridge.run_on_bridge([](UiaProvider& provider) {
        expect(provider.request_action(1, Action::SetValue, 1.0).has_value(), "first set-value");
        expect(provider.request_action(1, Action::SetValue, 2.0).has_value(), "second set-value");
        expect(provider.request_action(1, Action::Focus).has_value(), "focus");
    });
    if (!expect(both.has_value(), "actions queued")) {
        return false;
    }
    if (!expect_eq(static_cast<std::int64_t>(bridge.provider().queued_actions()), 2,
                   "set-value coalesced, focus kept")) {
        return false;
    }
    if (!expect_eq(static_cast<std::int64_t>(bridge.provider().coalesced_actions()), 1,
                   "one coalesced request")) {
        return false;
    }
    std::vector<UiaActionRequest> drained(4);
    const std::size_t taken = bridge.take_actions(drained);
    if (!expect_eq(static_cast<std::int64_t>(taken), 2, "two requests drained") ||
        !expect(drained[0].action == Action::SetValue, "set-value first") ||
        !expect(drained[0].value == 2.0, "newest value kept") ||
        !expect(drained[1].action == Action::Focus, "focus second")) {
        return false;
    }

    // Fill the bounded ring: every node is distinct, so nothing coalesces.
    std::vector<A11yNode> nodes;
    for (std::size_t index = 0; index < kMaxPendingUiaActions + 1; ++index) {
        A11yNode node = make_node(Role::Button, "Node");
        node.actions = Action::Focus;
        nodes.push_back(node);
    }
    bridge.publish(snapshot_of(std::move(nodes)));
    if (!expect(bridge.step().has_value(), "adopt the wide snapshot")) {
        return false;
    }
    UiaError last = UiaError::None;
    const auto overflow = bridge.run_on_bridge([&](UiaProvider& provider) {
        for (std::size_t index = 0; index <= kMaxPendingUiaActions; ++index) {
            const auto queued =
                provider.request_action(static_cast<A11yIndex>(index), Action::Focus);
            if (!queued) {
                last = queued.error();
                return;
            }
        }
    });
    return expect(overflow.has_value(), "the overflow run finished") &&
           expect(last == UiaError::QueueFull, "the ring is bounded with QueueFull") &&
           expect_eq(static_cast<std::int64_t>(bridge.provider().queued_actions()),
                     static_cast<std::int64_t>(kMaxPendingUiaActions), "ring holds its capacity");
}

} // namespace

int main() {
    if (!publication()) {
        return 1;
    }
    if (!overflow_policy()) {
        return 2;
    }
    if (!thread_ownership()) {
        return 3;
    }
    if (!action_coalescing_and_queue_bound()) {
        return 4;
    }
    std::cout << "a11y_uia_bridge: ok\n";
    return 0;
}
