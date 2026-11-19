// SPDX-License-Identifier: MIT

// ADR-056 slice 2 behavior, on the committed `Probe` base: mutating one cell re-runs exactly the
// bindings that read it (counted through TESSERA_BINDING_TRACE), a hand call of update() evaluates
// everything, and a missed-dependency guard compares the tree after each single-cell change with a
// full re-evaluation.

#define TESSERA_BINDING_TRACE
#include "Probe.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::vector<std::string> g_trace;

void record(const void*, const char* binding) {
    g_trace.emplace_back(binding);
}

std::string joined(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    std::string out;
    for (const std::string& value : values) {
        out += (out.empty() ? "" : ",") + value;
    }
    return out.empty() ? "<none>" : out;
}

// Everything the bindings write: text, class name, inline style, child kinds.
void snapshot(const ui::Node& node, std::string& out) {
    out += "[" + std::string(node.element_kind());
    if (node.paint_data()) {
        out += " text=" + node.paint_data()->text;
    }
    out += " class=" + std::string(node.class_name());
    for (const auto& style : node.inline_style()) {
        out += " style:" + std::string(style.name) + "=" + std::string(style.value);
    }
    for (const auto& child : node.children()) {
        snapshot(*child, out);
    }
    out += "]";
}

std::string snapshot_of(const ui::Node& root) {
    std::string out;
    snapshot(root, out);
    return out;
}

// One built, scheduler-attached, settled Probe.
struct Fixture {
    ui::Node root{1};
    demo::Probe probe;
    ui::UpdateScheduler scheduler;

    Fixture() {
        probe.build(root);
        probe.attach_scheduler(scheduler);
        probe.set_items({"x", "y"});
        scheduler.flush();
        g_trace.clear();
    }
    ~Fixture() {
        probe.detach_scheduler(scheduler);
    }

    // Runs `change`, flushes, and returns the sorted trace.
    std::string run(const std::function<void(demo::Probe&)>& change) {
        g_trace.clear();
        change(probe);
        scheduler.flush();
        return joined(g_trace);
    }
};

struct Case {
    std::string name;
    std::function<void(demo::Probe&)> change;
    std::string expected; // the bindings that must re-run
};

std::vector<Case> cases() {
    return {
        {"a", [](demo::Probe& p) { p.set_a(7); }, "list:7,text:2"},
        {"b", [](demo::Probe& p) { p.set_b(3); }, "meta:5:className,text:3"},
        {"tone", [](demo::Probe& p) { p.set_tone("blue"); }, "style:5:color"},
        {"flag", [](demo::Probe& p) { p.set_flag(true); }, "cond:6"},
        {"items", [](demo::Probe& p) { p.set_items({"x", "y", "z"}); }, "list:7"},
        {"title",
         [](demo::Probe& p) {
             p.prop_title_ = "hello";
             p.note_dirty(demo::Probe::kBit_prop_title_);
             p.schedule_update();
         },
         "text:4"},
    };
}

void test_exact_reruns() {
    for (const Case& c : cases()) {
        Fixture f;
        check(f.run(c.change) == c.expected, "cell `" + c.name + "` re-ran [" + joined(g_trace) +
                                                 "], expected [" + c.expected + "]");
    }
}

void test_no_change_runs_nothing() {
    Fixture f;
    check(f.run([](demo::Probe& p) { p.set_a(0); }) == "<none>",
          "an equal set marks nothing and runs no binding");
    check(f.run([](demo::Probe&) {}) == "<none>", "a flush with no change runs no binding");
}

void test_hand_update_evaluates_everything() {
    Fixture f;
    g_trace.clear();
    f.probe.update();
    check(joined(g_trace) == "cond:6,list:7,meta:5:className,style:5:color,text:2,text:3,text:4",
          "a hand call of update() evaluates every binding, got [" + joined(g_trace) + "]");
    g_trace.clear();
    f.probe.update();
    check(g_trace.size() == 7, "and does so again: update() always evaluates everything");
}

void test_first_build_evaluates_everything() {
    g_trace.clear();
    ui::Node root(1);
    demo::Probe probe;
    probe.build(root);
    check(g_trace.size() == 7,
          "the first build evaluates every binding, got [" + joined(g_trace) + "]");
}

// The missed-dependency guard: set every cell in turn through the scheduler and compare the tree
// with the one a hand-called, evaluate-everything update() produces from the same state.
void test_tree_matches_full_reevaluation() {
    for (const Case& c : cases()) {
        Fixture masked;
        masked.run(c.change);

        ui::Node reference_root(1);
        demo::Probe reference;
        reference.build(reference_root);
        reference.set_items({"x", "y"});
        c.change(reference);
        reference.update();
        check(snapshot_of(masked.root) == snapshot_of(reference_root),
              "cell `" + c.name + "`: the masked tree equals a full re-evaluation");
    }
    // Every cell changed in one flush.
    Fixture masked;
    ui::Node reference_root(1);
    demo::Probe reference;
    reference.build(reference_root);
    const auto all = [](demo::Probe& p) {
        p.set_a(2);
        p.set_b(5);
        p.set_tone("green");
        p.set_flag(true);
        p.set_items({"q"});
        p.prop_title_ = "t";
        p.note_dirty(demo::Probe::kBit_prop_title_);
        p.schedule_update();
    };
    masked.run(all);
    all(reference);
    reference.update();
    check(snapshot_of(masked.root) == snapshot_of(reference_root),
          "all cells changed together: the masked tree equals a full re-evaluation");
}

} // namespace

int main() {
    ui::g_binding_trace_hook = &record;
    test_exact_reruns();
    test_no_change_runs_nothing();
    test_hand_update_evaluates_everything();
    test_first_build_evaluates_everything();
    test_tree_matches_full_reevaluation();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen mask runtime tests passed\n";
    return 0;
}
