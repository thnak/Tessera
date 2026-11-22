// SPDX-License-Identifier: MIT

// ADR-056 slice 3 on the committed `Top` -> `Mid` -> `Leaf` bases: a parent never calls a child's
// update(); it writes the child's prop, marks its bit and schedules the child's own ticket, and the
// scheduler runs the child later in the same pass. Counted through UpdateFlushStats.

#include "Top.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <iostream>
#include <string>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// Every painted text of the tree in document order: `T:..|M:..|L:..`.
void texts(const ui::Node& node, std::string& out) {
    if (node.paint_data()) {
        out += (out.empty() ? "" : "|") + node.paint_data()->text;
    }
    for (const auto& child : node.children()) {
        texts(*child, out);
    }
}

std::string tree_text(const ui::Node& root) {
    std::string out;
    texts(root, out);
    return out;
}

struct Fixture {
    ui::Node root{1};
    demo::Top top;
    ui::UpdateScheduler scheduler;

    Fixture() {
        top.build(root);
        top.attach_scheduler(scheduler);
        scheduler.flush();
    }
    ~Fixture() {
        top.detach_scheduler(scheduler);
    }
};

void test_initial_tree() {
    Fixture f;
    check(tree_text(f.root) == "T:0|M:a:0|L:a",
          "the first build paints every level: " + tree_text(f.root));
}

// Parent -> child -> grandchild in one flush and one pass: three tickets, each once.
void test_prop_reaches_the_deepest_child() {
    Fixture f;
    f.top.set_label("b");
    const ui::UpdateFlushStats stats = f.scheduler.flush();
    check(tree_text(f.root) == "T:0|M:b:0|L:b",
          "the label reached the grandchild: " + tree_text(f.root));
    check(stats.updated == 3,
          "Top, Mid and Leaf each ran once, got " + std::to_string(stats.updated));
    check(stats.passes == 1,
          "the scheduled children ran in the same pass, got " + std::to_string(stats.passes));
}

// A parent change that feeds no child runs the parent only.
void test_unrelated_parent_state_runs_only_the_parent() {
    Fixture f;
    f.top.set_other(5);
    const ui::UpdateFlushStats stats = f.scheduler.flush();
    check(tree_text(f.root) == "T:5|M:a:0|L:a", "only the parent text changed");
    check(stats.updated == 1 && stats.passes == 1,
          "an owner change that feeds no child runs one ticket and one pass, got " +
              std::to_string(stats.updated) + "/" + std::to_string(stats.passes));
}

// A middle component's own state runs the middle only; its child's prop is unchanged.
void test_own_state_does_not_wake_the_child() {
    Fixture f;
    f.top.child_3().set_n(4);
    const ui::UpdateFlushStats stats = f.scheduler.flush();
    check(tree_text(f.root) == "T:0|M:a:4|L:a", "the middle text changed");
    check(stats.updated == 1,
          "a component's own change runs one ticket, got " + std::to_string(stats.updated));
}

// The owner and the child are both dirty: each runs once, in one pass.
void test_owner_and_child_dirty_run_once_each() {
    Fixture f;
    f.top.set_other(1);
    f.top.child_3().set_n(2);
    const ui::UpdateFlushStats stats = f.scheduler.flush();
    check(tree_text(f.root) == "T:1|M:a:2|L:a", "both texts changed");
    check(stats.updated == 2 && stats.passes == 1,
          "owner and child ran once each in one pass, got " + std::to_string(stats.updated) + "/" +
              std::to_string(stats.passes));
}

// A label change and the child's own change together: the child is scheduled by its own setter and
// by its parent's prop write, and still runs once.
void test_child_scheduled_twice_runs_once() {
    Fixture f;
    f.top.set_label("c");
    f.top.child_3().set_n(9);
    const ui::UpdateFlushStats stats = f.scheduler.flush();
    check(tree_text(f.root) == "T:0|M:c:9|L:c", "every level reflects both changes");
    check(stats.updated == 3 && stats.passes == 1, "Top, Mid and Leaf ran once each, got " +
                                                       std::to_string(stats.updated) + "/" +
                                                       std::to_string(stats.passes));
}

void test_idle_flushes_run_nothing() {
    Fixture f;
    check(f.scheduler.flush().updated == 0, "a flush with nothing queued runs nothing");
    f.top.set_label("a"); // equal to the current value: the setter marks nothing
    check(f.scheduler.flush().updated == 0, "an equal set runs nothing");
    f.top.set_label("z");
    f.scheduler.flush();
    check(f.scheduler.flush().updated == 0,
          "two flushes after one change run nothing the second time");
}

// A hand call of the parent's update() no longer cascades: it re-evaluates the parent only, and the
// child is reached through the scheduler like any other.
void test_hand_update_does_not_call_children() {
    Fixture f;
    f.top.child_3().prop_label_ = "hand";
    f.top.update();
    check(tree_text(f.root) == "T:0|M:a:0|L:a", "the parent's update() did not re-run the child");
}

} // namespace

int main() {
    test_initial_tree();
    test_prop_reaches_the_deepest_child();
    test_unrelated_parent_state_runs_only_the_parent();
    test_own_state_does_not_wake_the_child();
    test_owner_and_child_dirty_run_once_each();
    test_child_scheduled_twice_runs_once();
    test_idle_flushes_run_nothing();
    test_hand_update_does_not_call_children();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen chain runtime tests passed\n";
    return 0;
}
