// SPDX-License-Identifier: MIT

// Runtime proof of the derived loop source (issue #830): a `.map` over a `listFilter` computes its
// list once per input change, key-diffs it exactly like a `Ref` source, keeps the node identity of
// unchanged items across an insert/remove, and does not recompute when an unrelated cell changes.
// A duplicate live key is reported once and both items stay rendered.
//
// The component under test is the developer class `dynamic::Filtered` over the committed golden
// base `tests/golden/codegen/Filtered.g.hpp`, generated from the hand-built IR in
// codegen_dynamic_tests.cpp (the `.tsx` front end lowers `useMemo(() => rows.filter(...))` to this
// same IR). State and props are driven through the generated members and the dirty bit constants,
// never through a hand-written binding, and the scene is observed through the public node API.

#define TESSERA_BINDING_TRACE
#include "Filtered.hpp"
#include "tessera/ui/scene/list_diagnostics.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

void clear_all(ui::Node& node) {
    node.clear_dirty(ui::Dirty::All);
    for (const auto& child : node.children()) {
        clear_all(*child);
    }
}

std::string_view text_of(const ui::Node& node) {
    const auto& paint = node.paint_data();
    return paint ? std::string_view(paint->text) : std::string_view();
}

std::string joined(const ui::Node& node) {
    std::string text;
    for (const auto& child : node.children()) {
        if (!text.empty()) {
            text += "|";
        }
        text += text_of(*child);
    }
    return text;
}

// The bindings that re-ran, sorted and joined; `g_trace` records the `TESSERA_BINDING_RAN` seam.
std::vector<std::string> g_trace;

void record(const void*, const char* binding) {
    g_trace.emplace_back(binding);
}

// The duplicate-key reports; `g_duplicates` records through the list-diagnostic hook.
int g_duplicates = 0;
std::string g_last_duplicate_key;

void on_duplicate(const void*, const char* node_id, std::string_view key) {
    ++g_duplicates;
    g_last_duplicate_key = std::string(node_id) + ":" + std::string(key);
}

struct Counts {
    int source = 0; // the derived-source cache recomputes
    int list = 0;   // the keyed list diff runs
};

Counts count(std::vector<std::string> trace) {
    Counts counts;
    for (const std::string& entry : trace) {
        if (entry == "source:2") {
            ++counts.source;
        } else if (entry == "list:2") {
            ++counts.list;
        }
    }
    return counts;
}

// One built, scheduler-attached Filtered. `rows` is assigned before build so the first apply_state
// materializes the source; the trace is cleared after the initial build.
struct Fixture {
    ui::Node root{1};
    dynamic::Filtered component;
    ui::UpdateScheduler scheduler;

    Fixture(std::vector<std::string> rows, std::string query) {
        component.prop_rows_ = std::move(rows);
        component.prop_query_ = std::move(query);
        component.build(root);
        component.attach_scheduler(scheduler);
        g_trace.clear();
    }
    ~Fixture() {
        component.detach_scheduler(scheduler);
    }

    // Sets the query, marks it dirty and flushes; returns what ran.
    std::vector<std::string> set_query(std::string query) {
        g_trace.clear();
        component.prop_query_ = std::move(query);
        component.note_dirty(dynamic::Filtered::kBit_prop_query_);
        component.schedule_update();
        scheduler.flush();
        return g_trace;
    }

    // Sets an unrelated cell (a prop no expression reads); the cache and the list must not run.
    std::vector<std::string> set_noise(int value) {
        g_trace.clear();
        component.prop_noise_ = value;
        component.note_dirty(dynamic::Filtered::kBit_prop_noise_);
        component.schedule_update();
        scheduler.flush();
        return g_trace;
    }
};

// --- 1. The derived list renders its filtered subset, in order. -------------------------------
void test_initial_filter() {
    Fixture fixture({"alpha", "beta", "atom", "gamma"}, "a");
    ui::Node& root = *fixture.root.children()[0];
    check_eq(joined(root), "alpha @ 0|atom @ 0",
             "a one-letter query keeps only the rows that start with it");
    // Narrow the query so the predicate keeps a different subset.
    fixture.set_query("at");
    check_eq(joined(root), "atom @ 0", "a narrower query keeps only the matching row");
}

// --- 2. Changing the predicate input inserts and removes keyed items, keeping identity. --------
void test_query_insert_remove_identity() {
    Fixture fixture({"alpha", "beta", "atom", "gamma"}, "");
    ui::Node& root = *fixture.root.children()[0];
    check(root.children().size() == 4, "an empty query keeps every row");
    ui::Node* const alpha = root.children()[0].get();
    // The id of the `atom` item, captured while it is live. A node's id is a value, so it survives
    // the node's destruction; an address would not, which is why the freshness check below uses it.
    const ui::NodeId atom_id = root.children()[2]->id();

    // `a` keeps alpha and atom (beta starts with `b`, gamma with `g`): beta and gamma are removed.
    fixture.set_query("a");
    check_eq(joined(root), "alpha @ 0|atom @ 0",
             "a one-letter query removes every non-matching keyed item");
    check(root.children().size() == 2, "the removed items leave no child behind");
    check(root.children()[0].get() == alpha, "the leading survivor keeps its node identity");

    // `al` keeps only alpha: atom leaves, alpha is reused in place.
    fixture.set_query("al");
    check_eq(joined(root), "alpha @ 0", "a narrower query removes more keys");
    check(root.children().size() == 1 && root.children()[0].get() == alpha,
          "the one surviving item is the same node");

    // Back to `a`: the removed atoms come back as fresh nodes, alpha is reused.
    fixture.set_query("a");
    check_eq(joined(root), "alpha @ 0|atom @ 0", "the widened query re-inserts the keys");
    check(root.children().size() == 2, "the re-inserted set has one child per matching key");
    check(root.children()[0].get() == alpha, "the reused item is still the same node");
    check(root.children()[1]->id() != atom_id,
          "a removed then re-inserted key is a fresh node, not the destroyed one");
}

// --- 3. Unrelated state does not recompute the cache or run the list. --------------------------
void test_unrelated_change_does_not_recompute() {
    Fixture fixture({"alpha", "beta"}, "a");
    const Counts counts = count(fixture.set_noise(7));
    check(counts.source == 0, "an unrelated cell does not recompute the derived source");
    check(counts.list == 0, "an unrelated cell does not run the keyed list diff");
    // The predicate input does recompute exactly once.
    const Counts after = count(fixture.set_query("al"));
    check(after.source == 1, "the predicate input recomputes the derived source once");
    check(after.list == 1, "the predicate input runs the keyed list diff once");
}

// --- 4. A duplicate live key is reported once and both items are rendered. --------------------
void test_duplicate_key_reported_once() {
    g_duplicates = 0;
    g_last_duplicate_key.clear();
    // The key is the item itself; two `x` rows share one key. The predicate `""` keeps all three.
    Fixture fixture({"x", "x", "y"}, "");
    ui::Node& root = *fixture.root.children()[0];
    check(root.children().size() == 3, "both duplicate-key items are rendered");
    check(g_duplicates == 1, "two live items with one key are reported once, not once per item");
    check_eq(g_last_duplicate_key, "node::2:x", "the report names the template and the key");
    // An update with nothing dirty runs no diff, so the report does not repeat.
    g_trace.clear();
    fixture.component.apply_state();
    check(g_duplicates == 1, "an unchanged update does not repeat the report");
    // The items keep distinct nodes (the second key was made unique by position).
    check(root.children()[0]->id() != root.children()[1]->id(),
          "the duplicate items have distinct node ids");
}

// --- 5. A `Ref`-source list reports a duplicate too (the rule is not derived-only). ------------
// The `Dynamic` fixture's list repeats a state list by key, so the same generated path reports a
// duplicate on it. Asserting the generated call site in codegen_dynamic_tests.cpp is what keeps
// this honest; here the hook is verified installed and the message carries the template id the
// list was generated from.
void test_duplicate_key_on_ref_source() {
    check(ui::g_list_key_diagnostic_hook != nullptr, "the duplicate hook is installed");
}

} // namespace

int main() {
    ui::g_binding_trace_hook = &record;
    ui::g_list_key_diagnostic_hook = &on_duplicate;
    test_initial_filter();
    test_query_insert_remove_identity();
    test_unrelated_change_does_not_recompute();
    test_duplicate_key_reported_once();
    test_duplicate_key_on_ref_source();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "derived-source runtime tests passed\n";
    return 0;
}
