// SPDX-License-Identifier: MIT
//
// Dirty-driven publication and the tick-boundary action drain: a static window does no snapshot
// work, a dirty tick publishes exactly one tree, and client actions only run from the UI thread at
// a tick boundary, in arrival order, within the per-tick budget.

#include "a11y_uia_host_fixture.hpp"
#include "a11y_uia_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace a11y_uia_host_test;
using tessera::ui::a11y::A11yIndex;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::kMaxPendingUiaActions;
using tessera::ui::a11y::UiaActionRequest;
using tessera::ui::a11y::UiaHost;
using tessera::ui::a11y::UiaHostConfig;
using tessera::ui::a11y::UiaHostTickStats;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y uia tick: " << message << "\n";
    }
    return condition;
}

bool expect_eq(std::int64_t actual, std::int64_t expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y uia tick: " << message << " (actual=" << actual << ", expected=" << expected
              << ")\n";
    return false;
}

bool expect_text(std::string_view actual, std::string_view expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y uia tick: " << message << " (actual=\"" << actual << "\", expected=\""
              << expected << "\")\n";
    return false;
}

bool start_running(HostHarness& harness) {
    return expect(harness.host->start().has_value(), "start succeeds") &&
           expect(harness.host->bridge().step().has_value(), "the start hook has run") &&
           expect(harness.host->ready(), "the root is published");
}

// Publication-only tick for the cases that only need the tree handed to the bridge.
void publish_only(UiaHost& host, const UiaHost::SnapshotSource& source) {
    static_cast<void>(host.tick(source));
}

// Builds the two-node button tree most tests publish.
void build_button_tree(A11ySnapshot& out) {
    const auto snapshot = a11y_uia_test::button_snapshot("Save");
    out.nodes = snapshot.nodes;
}

void build_wide_focus_tree(A11ySnapshot& out) {
    out.clear();
    for (std::size_t index = 0; index < kMaxPendingUiaActions; ++index) {
        A11yNode node = a11y_uia_test::make_node(tessera::ui::a11y::Role::Button, "Node");
        node.actions = Action::Focus;
        out.nodes.push_back(node);
    }
}

bool static_tick_publishes_nothing() {
    auto harness = make_host();
    if (!start_running(*harness)) {
        return false;
    }
    std::size_t builds = 0;
    const auto source = [&](A11ySnapshot& out) {
        ++builds;
        build_button_tree(out);
    };
    const UiaHostTickStats stats = harness->host->tick(source);
    return expect(!stats.published, "a clean tick publishes nothing") &&
           expect(!stats.build_failed, "a clean tick builds nothing") &&
           expect_eq(static_cast<std::int64_t>(builds), 0, "the source is not called") &&
           expect_eq(static_cast<std::int64_t>(harness->host->bridge().handoff_stats().published),
                     0, "no snapshot reached the bridge") &&
           expect_eq(static_cast<std::int64_t>(stats.actions_applied), 0, "no actions") &&
           expect(!harness->host->dirty(), "the host stays clean");
}

bool dirty_tick_publishes_once() {
    auto harness = make_host();
    if (!start_running(*harness)) {
        return false;
    }
    std::size_t builds = 0;
    const auto source = [&](A11ySnapshot& out) {
        ++builds;
        build_button_tree(out);
    };
    harness->host->mark_dirty();
    if (!expect(harness->host->dirty(), "mark_dirty sets the flag")) {
        return false;
    }
    const UiaHostTickStats first = harness->host->tick(source);
    if (!expect(first.published, "a dirty tick publishes") ||
        !expect(!harness->host->dirty(), "publishing clears dirty") ||
        !expect_eq(static_cast<std::int64_t>(builds), 1, "one build") ||
        !expect_eq(static_cast<std::int64_t>(harness->host->bridge().handoff_stats().published), 1,
                   "one publish")) {
        return false;
    }
    const UiaHostTickStats second = harness->host->tick(source);
    return expect(!second.published, "the next tick is clean") &&
           expect_eq(static_cast<std::int64_t>(builds), 1, "still one build") &&
           expect_eq(static_cast<std::int64_t>(harness->host->bridge().handoff_stats().published),
                     1, "still one publish");
}

bool published_tree_reaches_the_provider() {
    auto harness = make_host();
    if (!start_running(*harness)) {
        return false;
    }
    harness->host->mark_dirty();
    const auto published = harness->host->tick([](A11ySnapshot& out) { build_button_tree(out); });
    if (!expect(published.published, "the dirty tick published") ||
        !expect(harness->host->bridge().step().has_value(), "barrier adopts the snapshot")) {
        return false;
    }
    std::size_t count = 0;
    std::string root_name;
    const bool served =
        harness->host->bridge()
            .run_on_bridge([&](tessera::ui::a11y::UiaProvider& provider) {
                count = provider.node_count();
                if (const auto* root = provider.node(provider.root()); root != nullptr) {
                    root_name.assign(tessera::ui::a11y::name_of(*root));
                }
            })
            .has_value();
    return expect(served, "the provider answers") &&
           expect_eq(static_cast<std::int64_t>(count), 2, "both nodes reached the provider") &&
           expect_text(root_name, "Root", "the root node is served");
}

bool actions_drain_at_the_tick_boundary() {
    auto harness = make_host();
    if (!start_running(*harness)) {
        return false;
    }
    // Two focusable children so the two actions have distinct node keys (they would coalesce on
    // the same node).
    harness->host->mark_dirty();
    publish_only(*harness->host, [](A11ySnapshot& out) {
        using namespace tessera::ui::a11y;
        A11yNode root = a11y_uia_test::make_node(Role::Generic, "Root");
        A11yNode first = a11y_uia_test::make_node(Role::Button, "First");
        A11yNode second = a11y_uia_test::make_node(Role::Button, "Second");
        first.actions = Action::Focus;
        second.actions = Action::Focus;
        a11y_uia_test::link_children(root, {1, 2});
        out.nodes.assign({root, first, second});
    });
    if (!expect(harness->host->bridge().step().has_value(), "barrier adopts the snapshot")) {
        return false;
    }
    const bool queued = harness->host->bridge()
                            .run_on_bridge([&](tessera::ui::a11y::UiaProvider& provider) {
                                return provider.request_action(1, Action::Focus).has_value() &&
                                       provider.request_action(2, Action::Focus).has_value();
                            })
                            .has_value();
    if (!expect(queued, "the bridge queued two actions")) {
        return false;
    }

    std::vector<A11yIndex> applied;
    const auto handler = [&](const UiaActionRequest& action) { applied.push_back(action.node); };
    const UiaHostTickStats stats = harness->host->tick([](A11ySnapshot&) {}, handler);
    return expect_eq(static_cast<std::int64_t>(stats.actions_applied), 2, "both actions applied") &&
           expect_eq(static_cast<std::int64_t>(stats.actions_deferred), 0, "nothing deferred") &&
           expect_eq(static_cast<std::int64_t>(applied.size()), 2, "handler saw both") &&
           expect_eq(static_cast<std::int64_t>(applied[0]), 1, "first action in arrival order") &&
           expect_eq(static_cast<std::int64_t>(applied[1]), 2, "second action in arrival order") &&
           expect_eq(static_cast<std::int64_t>(harness->host->bridge().queued_actions()), 0,
                     "the ring is drained");
}

bool drain_is_bounded_per_tick() {
    UiaHostConfig config = test_config();
    config.max_actions_per_tick = 4;
    auto harness = make_host(config);
    if (!start_running(*harness)) {
        return false;
    }
    harness->host->mark_dirty();
    publish_only(*harness->host, [](A11ySnapshot& out) { build_wide_focus_tree(out); });
    if (!expect(harness->host->bridge().step().has_value(), "barrier adopts the wide tree")) {
        return false;
    }
    bool queued_ok = false;
    const bool ran =
        harness->host->bridge()
            .run_on_bridge([&](tessera::ui::a11y::UiaProvider& provider) {
                queued_ok = true;
                for (std::size_t index = 0; index < kMaxPendingUiaActions; ++index) {
                    if (!provider.request_action(static_cast<A11yIndex>(index), Action::Focus)
                             .has_value()) {
                        queued_ok = false;
                        return;
                    }
                }
            })
            .has_value();
    if (!expect(ran && queued_ok, "the whole bounded ring was queued")) {
        return false;
    }

    std::size_t applied = 0;
    const auto handler = [&](const UiaActionRequest&) { ++applied; };
    std::size_t ticks = 0;
    std::size_t deferred = kMaxPendingUiaActions;
    while (deferred > 0 && ticks < kMaxPendingUiaActions) {
        const UiaHostTickStats stats = harness->host->tick([](A11ySnapshot&) {}, handler);
        if (!expect_eq(static_cast<std::int64_t>(stats.actions_applied), 4,
                       "each tick applies exactly its budget")) {
            return false;
        }
        deferred = stats.actions_deferred;
        ++ticks;
    }
    return expect_eq(static_cast<std::int64_t>(applied),
                     static_cast<std::int64_t>(kMaxPendingUiaActions), "every action ran once") &&
           expect_eq(static_cast<std::int64_t>(deferred), 0, "the ring ended empty") &&
           expect_eq(static_cast<std::int64_t>(ticks), kMaxPendingUiaActions / 4,
                     "the bound sets the number of ticks");
}

bool null_handler_keeps_actions_queued() {
    auto harness = make_host();
    if (!start_running(*harness)) {
        return false;
    }
    harness->host->mark_dirty();
    publish_only(*harness->host, [](A11ySnapshot& out) { build_button_tree(out); });
    if (!expect(harness->host->bridge().step().has_value(), "barrier adopts the snapshot")) {
        return false;
    }
    const bool queued = harness->host->bridge()
                            .run_on_bridge([&](tessera::ui::a11y::UiaProvider& provider) {
                                return provider.request_action(1, Action::Focus).has_value();
                            })
                            .has_value();
    if (!expect(queued, "one action queued")) {
        return false;
    }
    const UiaHostTickStats stats = harness->host->tick([](A11ySnapshot&) {});
    return expect_eq(static_cast<std::int64_t>(stats.actions_applied), 0, "nothing applied") &&
           expect_eq(static_cast<std::int64_t>(stats.actions_deferred), 1, "one deferred") &&
           expect_eq(static_cast<std::int64_t>(harness->host->bridge().queued_actions()), 1,
                     "the action was not discarded");
}

bool throwing_source_keeps_the_previous_tree() {
    auto harness = make_host();
    if (!start_running(*harness)) {
        return false;
    }
    harness->host->mark_dirty();
    publish_only(*harness->host, [](A11ySnapshot& out) { build_button_tree(out); });
    if (!expect(harness->host->bridge().step().has_value(), "barrier adopts the first tree")) {
        return false;
    }
    harness->host->mark_dirty();
    const auto failed = harness->host->tick(
        [](A11ySnapshot&) { throw std::runtime_error("host snapshot failure"); });
    if (!expect(failed.build_failed, "the failure is reported as a value") ||
        !expect(!failed.published, "a failed build publishes nothing") ||
        !expect(harness->host->dirty(), "the host stays dirty for a retry")) {
        return false;
    }
    // The previous tree is still served.
    std::size_t count = 0;
    const bool served = harness->host->bridge()
                            .run_on_bridge([&](tessera::ui::a11y::UiaProvider& provider) {
                                count = provider.node_count();
                            })
                            .has_value();
    const auto retried = harness->host->tick([](A11ySnapshot& out) { build_button_tree(out); });
    return expect(served, "the provider still answers") &&
           expect_eq(static_cast<std::int64_t>(count), 2, "the previous tree is still served") &&
           expect(retried.published, "the retry publishes") &&
           expect(!harness->host->dirty(), "the retry clears dirty");
}

bool throwing_handler_is_reported() {
    auto harness = make_host();
    if (!start_running(*harness)) {
        return false;
    }
    harness->host->mark_dirty();
    publish_only(*harness->host, [](A11ySnapshot& out) { build_button_tree(out); });
    if (!expect(harness->host->bridge().step().has_value(), "barrier adopts the snapshot")) {
        return false;
    }
    const bool queued = harness->host->bridge()
                            .run_on_bridge([&](tessera::ui::a11y::UiaProvider& provider) {
                                return provider.request_action(1, Action::Focus).has_value();
                            })
                            .has_value();
    if (!expect(queued, "one action queued")) {
        return false;
    }
    const auto stats = harness->host->tick(
        [](A11ySnapshot&) {},
        [](const UiaActionRequest&) { throw std::runtime_error("action failure"); });
    return expect_eq(static_cast<std::int64_t>(stats.actions_applied), 0, "no action completed") &&
           expect_eq(static_cast<std::int64_t>(stats.actions_failed), 1, "one action failed") &&
           expect_eq(static_cast<std::int64_t>(stats.actions_deferred), 0,
                     "the failed action is dropped, not retried");
}

} // namespace

int main() {
    if (!static_tick_publishes_nothing()) {
        return 1;
    }
    if (!dirty_tick_publishes_once()) {
        return 2;
    }
    if (!published_tree_reaches_the_provider()) {
        return 3;
    }
    if (!actions_drain_at_the_tick_boundary()) {
        return 4;
    }
    if (!drain_is_bounded_per_tick()) {
        return 5;
    }
    if (!null_handler_keeps_actions_queued()) {
        return 6;
    }
    if (!throwing_source_keeps_the_previous_tree()) {
        return 7;
    }
    if (!throwing_handler_is_reported()) {
        return 8;
    }
    std::cout << "a11y_uia_host_tick: ok\n";
    return 0;
}
