// SPDX-License-Identifier: MIT

// End-to-end runtime proof of the per-branch and per-item component instance model (issue #231,
// docs/design/compiler-node-state.md) on the ADR-040 generated base. The generated sample is the
// committed `instances/{Page,Row}.g.hpp`; the developer classes live in
// tests/unit/codegen_base/fixtures/instances. No GPU.
//
// Covered: a keyed list of component instances (one instance per item, state follows the key across
// a reorder, a removed key unmounts), a conditional component instance (one instance, keep-alive
// branch slot), a conditional inside a list item (per-item presence), a list-typed prop mapped
// inside the callee, and the `(template uiId, item key)` automation identity.
//
// The item records themselves (`list_N_items_[i]`, `Item_<n>::child_<n>`) are generated internal
// bookkeeping (ADR-040 keeps them private), so this test observes the same facts through the scene:
// the item host nodes ARE the caller's children, in source order. The structure is asserted
// explicitly before any index is used, so a change in child order fails loudly instead of silently
// testing the wrong nodes.

#include "Page.hpp"
#include "Row.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
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

std::string_view painted_text(const ui::Node& node) {
    const auto& paint = node.paint_data();
    if (!paint.has_value()) {
        return {};
    }
    return paint->text;
}

// The text of one `Row` instance: the host's only child is the callee root, whose first child is
// the generated `{label}:{count}` text node.
std::string_view row_text(const ui::Node& host) {
    const auto& children = host.children();
    if (children.empty() || children[0]->children().empty()) {
        return {};
    }
    return painted_text(*children[0]->children()[0]);
}

// The callee root of one `Row` instance (the host's only child).
const ui::Node* row_root(const ui::Node& host) {
    const auto& children = host.children();
    return children.empty() ? nullptr : children[0].get();
}

// The page root: build() attaches the page's own root view under the scene root.
// Child order of that view, asserted below: the two per-item presence items first, then the three
// component-instance hosts, all in source order.
constexpr std::size_t kPresenceCount = 2;
constexpr std::size_t kInstanceCount = 3;
constexpr std::size_t kChildCount = kPresenceCount + kInstanceCount;

ui::Node& root_view(ui::Node& scene_root) {
    return *scene_root.children()[0];
}

// The presence item at `index` (they come first, in source order).
ui::Node& presence_item(ui::Node& view, std::size_t index) {
    return *view.children()[index];
}

// The component-instance host at `index` (they come after the presence items).
ui::Node& instance_host(ui::Node& view, std::size_t index) {
    return *view.children()[kPresenceCount + index];
}

// Asserts the child layout the rest of this file indexes into.
void check_view_structure(ui::Node& view, std::size_t instances,
                          std::string_view what) {
    check(view.children().size() == kPresenceCount + instances,
          std::string(what) + ": the view holds the presence items and the instance hosts");
    for (std::size_t index = 0; index < kPresenceCount && index < view.children().size(); ++index) {
        check(presence_item(view, index).element_kind() == "View",
              std::string(what) + ": a presence item is a View, as the template declares");
      }
    for (std::size_t index = 0; index < instances; ++index) {
        const std::size_t at = kPresenceCount + index;
        if (at >= view.children().size()) {
            continue;
        }
        const ui::Node& host = *view.children()[at];
        check(host.element_kind() == "Component",
              std::string(what) + ": an instance slot is a Component host");
        check(host.component_name() == "Row",
              std::string(what) + ": the host names the callee");
    }
}

void test_keyed_list_of_instances() {
    demo::Page page;
    ui::Node root(1000);
    page.build(root);

    check(root.children().size() == 1, "the page builds one root child");
    ui::Node& view = root_view(root);
    check(view.children().size() == kChildCount,
          "two presence items and three component instances are attached in source order");
    check_view_structure(view, kInstanceCount, "initial build");
    if (view.children().size() != kChildCount) {
        return;
    }

    // One `Row` instance per key, each attached at its own position with its own host node and
    // automation identity.
    for (std::size_t index = 0; index < kInstanceCount; ++index) {
        ui::Node& host = instance_host(view, index);
        check(row_root(host) != nullptr, "the host has its callee root attached");
        check(host.automation_id() == 2104, "the host carries the template's automation id");
    }

    // The `Row` maps its list-typed prop: one generated row item per `notes` element, painted with
    // the element the caller passed for that item.
    const std::string_view entries[] = {"a", "b", "c"};
    for (std::size_t index = 0; index < kInstanceCount; ++index) {
        ui::Node& host = instance_host(view, index);
        const ui::Node* callee = row_root(host);
        check(callee != nullptr, "the instance has its callee root");
        if (callee == nullptr) {
            continue;
        }
        if (callee->children().size() < 2) {
            check(false, "the callee has its text and its list-typed prop container");
            continue;
        }
        const ui::Node& list_root = *callee->children()[1];
        check(list_root.children().size() == 1, "the list-typed prop produced one row");
        if (!list_root.children().empty()) {
            check(painted_text(*list_root.children()[0]) == entries[index],
                  "the list-typed prop element paints the caller's value");
        }
    }
    check(row_text(instance_host(view, 0)) == "a:0", "the first instance paints its props");
    check(row_text(instance_host(view, 1)) == "b:0", "the second instance paints its props");

    // Presence per item: `i > 0` attaches the item's text node for every item but the first. The
    // item key is scene metadata on the item root.
    // The initial `nums` is {10, 20}: two presence items. `i > 0` attaches a text node for every
    // item but the first.
    check(view.children().size() > kPresenceCount,
          "the initial presence items precede the instance hosts");
    for (std::size_t index = 0; index < kPresenceCount; ++index) {
        ui::Node& item = presence_item(view, index);
        check(item.children().size() == index, "the per-item condition attaches per item");
        check(item.item_key().has_value(), "a numeric key publishes the item key");
        if (item.item_key().has_value()) {
            check(*item.item_key() == index, "the item key is the loop index");
        }
    }
}

// Instance state belongs to the key: a reorder moves the record, never the state.
//
// A keyed item's `Row` instance is template-owned (one per key inside the item record), so it has no
// `child_<n>()` accessor and its state cell cannot be written from outside. The claims this test can
// and does prove through the scene:
//   - the key's host node moves with the key (node identity across the reorder);
//   - the callee subtree under that host is preserved, not rebuilt (the callee root node keeps its
//     address, which a rebuild would change);
//   - the keys' rendered content follows them, and other keys are untouched;
//   - a removed key drops its host, the kept key keeps its callee.
// Instance *state* survival across a reorder is proven on the conditional instance
// (`test_conditional_instance_keeps_state`), which is a caller-owned member with a public accessor.
void test_state_survives_reorder() {
    demo::Page page;
    ui::Node root(1000);
    page.build(root);
    ui::Node& view = root_view(root);
    check_view_structure(view, kInstanceCount, "reorder: initial");
    if (view.children().size() != kChildCount) {
        return;
    }

    // Key "c" is the third instance. Capture its host and callee root before the reorder.
    ui::Node* const host_c = &instance_host(view, 2);
    const ui::Node* const callee_c = row_root(*host_c);
    check(callee_c != nullptr, "key c has a callee root before the reorder");
    check(row_text(*host_c) == "c:0", "key c paints its label");

    page.set_entries(std::vector<std::string>{"c", "a", "b"});
    page.update();

    check(view.children().size() == kChildCount, "the reorder keeps the child count");
    check_view_structure(view, kInstanceCount, "reorder: after");
    check(&instance_host(view, 0) == host_c, "the key's host node moved with the key");
    check(row_root(*host_c) == callee_c,
          "the callee subtree was preserved, not rebuilt (same root node)");
    check(row_text(*host_c) == "c:0", "the reordered instance still paints its own key");
    check(row_text(instance_host(view, 1)) == "a:0", "the other keys are untouched");

    // A removed key unmounts its instance and drops the record.
    page.set_entries(std::vector<std::string>{"a"});
    page.update();
    check(view.children().size() == kPresenceCount + 1, "the removed keys are gone");
    check_view_structure(view, 1, "reorder: after removal");
    check(row_root(instance_host(view, 0)) != nullptr, "the kept key keeps its instance");
    check(row_text(instance_host(view, 0)) == "a:0", "the kept key keeps its content");
}

// A conditional instance is one instance in a branch slot: toggling moves the host node, and the
// instance (state, effects) survives both directions. The keep-alive claim is proven by capturing
// the host's address while attached and requiring the same address back after a full toggle.
void test_conditional_instance_keeps_state() {
    demo::Page page;
    ui::Node root(1000);
    page.build(root);
    ui::Node& view = root_view(root);
    check(view.children().size() == kChildCount,
          "the conditional instance starts detached");
    if (view.children().size() != kChildCount) {
        return;
    }

    page.set_show(true);
    page.update();
    check(view.children().size() == kChildCount + 1, "showing the condition attaches the instance");
    ui::Node* const host = &*view.children().back();
    check(host->element_kind() == "Component", "the branch attaches a Component host");
    check(row_text(*host) == "cond:0", "the conditional instance paints its props");

    page.child_5().set_count(5);
    page.update();
    check(row_text(*host) == "cond:5", "the conditional instance has its own state");

    page.set_show(false);
    page.update();
    check(view.children().size() == kChildCount, "hiding the condition detaches the host");

    page.set_show(true);
    page.update();
    check(view.children().size() == kChildCount + 1, "re-showing reattaches the host");
    check(&*view.children().back() == host,
          "re-showing reattaches the same host node (keep-alive, not rebuild)");
    check(row_text(*host) == "cond:5", "the instance state survived the toggle");
    // The callee mapped its empty list-typed prop into no children.
    const ui::Node* callee = row_root(*host);
    check(callee != nullptr, "the conditional instance has its callee root");
    // `notes={[]}` renders no list rows: the callee attaches its `{label}:{count}` text and no
    // list container children.
    check(callee != nullptr && callee->children().size() == 1,
          "the conditional instance maps its empty list prop to no rows");
    if (callee != nullptr && !callee->children().empty()) {
        check_eq(painted_text(*callee->children()[0]), "cond:5",
                 "the conditional instance paints its label and state");
    }
}

// Per-item presence: changing the list re-runs each item's own condition walk.
void test_conditional_inside_item() {
    demo::Page page;
    ui::Node root(1000);
    page.build(root);
    ui::Node& view = root_view(root);

    page.set_nums(std::vector<int>{5});
    page.update();
    // The presence list is keyed over `nums`, so its item count follows the data source; the three
    // component-instance hosts are unaffected.
    check(view.children().size() == 1 + kInstanceCount,
          "the presence list shrank to one item; the instance hosts stay");
    check(presence_item(view, 0).children().empty(),
          "the first item's condition is false and attaches nothing");

    page.set_nums(std::vector<int>{5, 6, 7});
    page.update();
    check(view.children().size() == 3 + kInstanceCount,
          "the presence list grew to three items; the instance hosts stay");
    check(presence_item(view, 0).children().empty(),
          "the first item still attaches nothing");
    const std::string_view numeric_text[] = {"", "6", "7"};
    for (std::size_t index = 1; index < 3; ++index) {
        ui::Node& item = presence_item(view, index);
        if (item.children().empty()) {
            check(false, "a later item attaches its conditional text");
            continue;
        }
        check(painted_text(*item.children()[0]) == numeric_text[index],
              "the per-item text paints the item's value");
    }
}

// The owner's lifecycle drives the live item instances.
void test_owner_lifecycle() {
    demo::Page page;
    ui::Node root(1000);
    page.build(root);
    ui::Node& view = root_view(root);

    // build() does not mount the owner; an item created afterwards is live because the owner is
    // mounted. The mounted flag itself is generated instrumentation with no observable claim; the
    // observable consequence is that a late item still gets its callee built and attached.
    page.mount();
    page.set_entries(std::vector<std::string>{"a", "b"});
    page.update();
    check_view_structure(view, 2, "owner lifecycle: after mount");
    check(view.children().size() == kPresenceCount + 2, "an item created after mount is live");
    check(row_root(instance_host(view, 0)) != nullptr, "a late item has its instance");
    check(row_text(instance_host(view, 0)) == "a:0", "a late item paints its props");
    page.unmount();
}

} // namespace

int main() {
    test_keyed_list_of_instances();
    test_state_survives_reorder();
    test_conditional_instance_keeps_state();
    test_conditional_inside_item();
    test_owner_lifecycle();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen instance runtime tests passed\n";
    return 0;
}
