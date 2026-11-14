// SPDX-License-Identifier: MIT

// End-to-end runtime proof of the per-branch and per-item component instance model (issue #231,
// docs/design/compiler-node-state.md). The generated sample is the stored golden of
// `codegen_instances_tests --write`; the build compiles it into `tessera_codegen_instances_sample`.
// No GPU.
//
// Covered: a keyed list of component instances (one instance per item, state follows the key across
// a reorder, a removed key unmounts), a conditional component instance (one instance, keep-alive
// branch slot), a conditional inside a list item (per-item presence), a list-typed prop mapped
// inside the callee, and the `(template uiId, item key)` automation identity.

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

std::string_view text_of(std::string_view value) {
    return value;
}

// The page root: build() attaches the page's own root view under the scene root.
void test_keyed_list_of_instances() {
    demo::gen::Page page;
    ui::Node root(1000);
    page.build(root);

    check(root.children().size() == 1, "the page builds one root child");
    ui::Node& view = *root.children()[0];
    check(view.children().size() == 5,
          "two presence items and three component instances are attached in source order");

    // The keyed list of instances owns three `Row` instances, one per item.
    check(page.list_4_items_.size() == 3, "one item record per key");
    for (std::size_t index = 0; index < page.list_4_items_.size(); ++index) {
        demo::gen::Item_4& item = page.list_4_items_[index];
        check(item.root != nullptr, "the item record owns its root node");
        check(item.child_4 != nullptr, "the item record owns its instance");
        check(item.root == view.children()[2 + index].get(),
              "the instance host is attached at the item's position");
        check(item.root->element_kind() == "Component", "the item root is the instance host");
        check(item.root->component_name() == "Row", "the host names the callee");
        check(item.root->automation_id() == 2104, "the host carries the template's automation id");
    }

    // The `Row` maps its list-typed prop: one generated row item per `notes` element, painted with
    // the element the caller passed for that item.
    const std::string_view entries[] = {"a", "b", "c"};
    for (std::size_t index = 0; index < page.list_4_items_.size(); ++index) {
        const demo::gen::Row& row = *page.list_4_items_[index].child_4;
        check(row.list_3_items_.size() == 1, "the callee materialized its list-typed prop");
        const ui::Node* host = page.list_4_items_[index].root;
        check(host != nullptr && row_root(*host) != nullptr, "the instance has its callee root");
        if (host != nullptr && row_root(*host) != nullptr) {
            const ui::Node& list_root = *row_root(*host)->children()[1];
            check(list_root.children().size() == 1, "the list-typed prop produced one row");
            check(painted_text(*list_root.children()[0]) == entries[index],
                  "the list-typed prop element paints the caller's value");
        }
    }
    check(row_text(*page.list_4_items_[0].root) == "a:0", "the first instance paints its props");
    check(row_text(*page.list_4_items_[1].root) == "b:0", "the second instance paints its props");

    // Presence per item: `i > 0` attaches the item's text node for every item but the first.
    check(page.list_2_items_.size() == 2, "one presence record per numeric item");
    for (std::size_t index = 0; index < page.list_2_items_.size(); ++index) {
        demo::gen::Item_2& item = page.list_2_items_[index];
        check(item.root != nullptr, "the presence item has its view root");
        check(item.root->children().size() == index, "the per-item condition attaches per item");
        check(item.root->item_key().has_value(), "a numeric key publishes the item key");
        if (item.root->item_key().has_value()) {
            check(*item.root->item_key() == index, "the item key is the loop index");
        }
    }
}

// Instance state belongs to the key: a reorder moves the record, never the state.
void test_state_survives_reorder() {
    demo::gen::Page page;
    ui::Node root(1000);
    page.build(root);

    demo::gen::Item_4& third = page.list_4_items_[2]; // key "c"
    ui::Node* const host_c = third.root;
    demo::gen::Row* const instance_c = third.child_4.get();
    instance_c->set_count(7);
    page.update();
    check(row_text(*host_c) == "c:7", "the instance state paints through the callee");

    page.set_entries(std::vector<std::string>{"c", "a", "b"});
    page.update();

    check(page.list_4_items_.size() == 3, "the reorder keeps the item count");
    check(page.list_4_items_[0].root == host_c, "the key's host node moved with the key");
    check(page.list_4_items_[0].child_4.get() == instance_c,
          "the key's instance moved with the key (state survives)");
    check(row_text(*host_c) == "c:7", "the reordered instance still paints its state");
    check(row_text(*page.list_4_items_[1].root) == "a:0", "the other keys are untouched");

    // A removed key unmounts its instance and drops the record.
    page.set_entries(std::vector<std::string>{"a"});
    page.update();
    check(page.list_4_items_.size() == 1, "the removed keys are gone");
    check(page.list_4_items_[0].child_4 != nullptr, "the kept key keeps its instance");
    check(row_text(*page.list_4_items_[0].root) == "a:0", "the kept key keeps its state");
}

// A conditional instance is one instance in a branch slot: toggling moves the host node, and the
// instance (state, effects) survives both directions.
void test_conditional_instance_keeps_state() {
    demo::gen::Page page;
    ui::Node root(1000);
    page.build(root);
    ui::Node& view = *root.children()[0];
    check(view.children().size() == 5, "the conditional instance starts detached");
    check(page.branch_5_owned_ != nullptr, "the conditional instance host exists while detached");
    ui::Node* const host = page.branch_5_owned_.get();

    page.set_show(true);
    page.update();
    check(view.children().size() == 6, "showing the condition attaches the instance host");
    check(view.children().back().get() == host, "the branch attaches at its source position");
    check(row_text(*host) == "cond:0", "the conditional instance paints its props");

    page.child_5_.set_count(5);
    page.update();
    check(row_text(*host) == "cond:5", "the conditional instance has its own state");

    page.set_show(false);
    page.update();
    check(view.children().size() == 5, "hiding the condition detaches the host");
    check(page.branch_5_owned_.get() == host, "the detached host stays owned by its slot");

    page.set_show(true);
    page.update();
    check(view.children().back().get() == host, "re-showing reattaches the same host node");
    check(row_text(*host) == "cond:5", "the instance state survived the toggle");
    check(page.child_5_.list_3_items_.empty(), "the conditional instance maps its empty list prop");
}

// Per-item presence: changing the list re-runs each item's own condition walk.
void test_conditional_inside_item() {
    demo::gen::Page page;
    ui::Node root(1000);
    page.build(root);

    page.set_nums(std::vector<int>{5});
    page.update();
    check(page.list_2_items_.size() == 1, "the presence list shrank");
    check(page.list_2_items_[0].root->children().empty(),
          "the first item's condition is false and attaches nothing");

    page.set_nums(std::vector<int>{5, 6, 7});
    page.update();
    check(page.list_2_items_.size() == 3, "the presence list grew");
    check(page.list_2_items_[0].root->children().empty(), "the first item still attaches nothing");
    const std::string_view numeric_text[] = {"", "6", "7"};
    for (std::size_t index = 1; index < page.list_2_items_.size(); ++index) {
        demo::gen::Item_2& item = page.list_2_items_[index];
        check(item.root->children().size() == 1, "a later item attaches its conditional text");
        if (item.root->children().empty()) {
            continue;
        }
        check(item.texts[0] == item.root->children()[0].get(),
              "the attached text node is the one the item record owns");
        check(painted_text(*item.root->children()[0]) == numeric_text[index],
              "the per-item text paints the item's value");
    }
}

// The owner's lifecycle drives the live item instances.
void test_owner_lifecycle() {
    demo::gen::Page page;
    ui::Node root(1000);
    page.build(root);
    check(!page.mounted_, "build() does not mount the owner");
    page.mount();
    check(page.mounted_, "mount() marks the owner mounted");
    page.set_entries(std::vector<std::string>{"a", "b"});
    page.update();
    check(page.list_4_items_.size() == 2, "an item created after mount is live");
    check(page.list_4_items_[0].child_4 != nullptr, "a late item has its instance");
    page.unmount();
    check(!page.mounted_, "unmount() clears the mounted flag");
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
