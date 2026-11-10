// SPDX-License-Identifier: MIT

// Runtime proof of the conditional + keyed-list codegen contract (docs/design/compiler.md section
// 11): the generated component builds every conditional branch once and attaches/detaches it at its
// deterministic source position, and the bounded list item table reuses nodes by key, reorders
// in place, truncates at the documented cap, and marks nothing dirty when nothing changed.
//
// The component under test is the committed golden `tests/golden/codegen/Dynamic.cpp`, generated
// from the hand-built IR in codegen_dynamic_tests.cpp (the `.tsx` front end cannot author a list
// type yet).

#include "Dynamic.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
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

std::vector<std::string_view> texts_of(const ui::Node& node) {
    std::vector<std::string_view> texts;
    for (const auto& child : node.children()) {
        texts.push_back(text_of(*child));
    }
    return texts;
}

std::string joined(const ui::Node& node) {
    std::string text;
    for (const std::string_view part : texts_of(node)) {
        if (!text.empty()) {
            text += "|";
        }
        text += part;
    }
    return text;
}

bool true_at_dirty(const ui::Node& node) {
    return node.dirty() == ui::Dirty::Clean;
}

} // namespace

int main() {
    dynamic::gen::Dynamic component;
    component.state_items_ = {"beta", "alpha"};
    ui::Node scene(1000);
    component.build(scene);

    check(scene.children().size() == 1, "the component builds exactly one root child");
    ui::Node& root = *scene.children()[0];

    // --- 1. Initial child set: static children + list items, conditional branches detached.
    // -------
    check(component.list_5_count_ == 2, "the keyed list materialized both items");
    check(root.children().size() == 7, "the initial child set has the static children and items");
    if (root.children().size() == 7) {
        check_eq(joined(root), "Header|Count: 0|beta @ 0|alpha @ 0|||Footer",
                 "static children, list items and the present conditional branch in source order");
        check_eq(root.children()[4]->element_kind(), "View",
                 "the present ternary branch is the View");
        check(root.children()[4]->children().size() == 1 &&
                  text_of(*root.children()[4]->children()[0]) == "Empty",
              "the present ternary branch keeps its child subtree");
    }
    check(component.branch_3_node_ == nullptr && component.branch_3_owned_ != nullptr,
          "the `count > 0` branch starts detached but built");
    check(component.branch_6_node_ != nullptr, "the `count === 0` branch starts attached");
    check(component.branch_8_node_ == nullptr, "the `count !== 0` branch starts detached");
    check(component.list_5_items_[0].root->id() == 5 && component.list_5_items_[1].root->id() == 18,
          "item node ids are the template id plus the item index times kItemIdStride");

    // --- 2. Toggle: the branch attaches at its source position, the ternary swaps. --------------
    clear_all(root);
    component.state_count_ = 1;
    component.apply_state();
    check(root.children().size() == 8, "the toggled child set has one more child");
    if (root.children().size() == 8) {
        check_eq(joined(root), "Header|Positive|Count: 1|beta @ 1|alpha @ 1|||Footer",
                 "the attached branch sits at its source position and the ternary swapped");
        check_eq(root.children()[1]->element_kind(), "Text", "the attached branch is the Text");
        check_eq(root.children()[5]->children()[0]->paint_data()->text, "Filled",
                 "the complementary ternary branch is now present");
    }
    check(component.branch_3_node_ != nullptr && component.branch_6_node_ == nullptr &&
              component.branch_8_node_ != nullptr,
          "the present/absent slots follow the condition after the toggle");
    check(ui::has_flag(root.dirty(), ui::Dirty::Layout) &&
              ui::has_flag(root.dirty(), ui::Dirty::Paint) &&
              ui::has_flag(root.dirty(), ui::Dirty::Render),
          "a child-set change marks Layout|Paint|Render on the shared parent");
    check(ui::has_flag(component.branch_3_node_->dirty(), ui::Dirty::Layout) &&
              ui::has_flag(component.branch_3_node_->dirty(), ui::Dirty::Paint),
          "an attached branch is marked dirty in the layout and paint domains");
    check(!ui::has_flag(root.dirty(), ui::Dirty::Style),
          "a child-set change never marks the selector domain");

    // --- 3. No-change update: nothing dirty, same table, same hashes. ---------------------------
    clear_all(root);
    const std::uint64_t order_before = component.list_5_order_hash_;
    component.apply_state();
    check(true_at_dirty(root), "an unchanged update marks no dirty domain on the root");
    check(component.list_5_order_hash_ == order_before, "an unchanged key list keeps its hash");
    check(root.children().size() == 8, "an unchanged update does not rebuild children");

    // --- 4. Reorder: nodes are reused by key, only the positions change. ------------------------
    ui::Node* const beta = component.list_5_items_[0].root;
    ui::Node* const alpha = component.list_5_items_[1].root;
    clear_all(root);
    component.state_items_ = {"alpha", "beta"};
    component.apply_state();
    check(component.list_5_items_[0].root == alpha && component.list_5_items_[1].root == beta,
          "a reorder reuses the item nodes by key");
    check_eq(joined(root), "Header|Positive|Count: 1|alpha @ 1|beta @ 1|||Footer",
             "the reordered items paint in the source order");
    check(text_of(*alpha) == "alpha @ 1" && text_of(*beta) == "beta @ 1",
          "reused item content is unchanged by the reorder");

    // --- 5. Remove and insert: stale nodes leave, new keys build one node each. ----------------
    clear_all(root);
    component.state_items_ = {"alpha"};
    component.apply_state();
    check(component.list_5_count_ == 1 && root.children().size() == 7,
          "removing a key drops one item");
    check_eq(joined(root), "Header|Positive|Count: 1|alpha @ 1|||Footer",
             "the removed item leaves no child behind");
    clear_all(root);
    component.state_items_ = {"alpha", "gamma", "beta"};
    component.apply_state();
    check(component.list_5_count_ == 3, "inserting keys grows the item table");
    check(component.list_5_items_[0].root == alpha,
          "the surviving key keeps its node across an insert");
    check(component.list_5_items_[0].root != component.list_5_items_[1].root &&
              component.list_5_items_[1].root != component.list_5_items_[2].root,
          "each key owns a distinct item node");
    check_eq(joined(root), "Header|Positive|Count: 1|alpha @ 1|gamma @ 1|beta @ 1|||Footer",
             "the inserted key is materialized in order");

    // --- 6. Content-only change: same keys, new text, no structural work. ----------------------
    clear_all(root);
    const std::uint64_t content_order = component.list_5_order_hash_;
    component.state_count_ = 2;
    component.apply_state();
    check(component.list_5_order_hash_ == content_order,
          "a state change does not change the key order hash");
    check(component.list_5_items_[0].root == alpha, "a content-only change keeps the item nodes");
    check_eq(joined(root), "Header|Positive|Count: 2|alpha @ 2|gamma @ 2|beta @ 2|||Footer",
             "the per-item content pass repainted every item text");
    check(root.dirty() == ui::Dirty::Clean,
          "a content-only item change marks only the item text, not the parent");
    check(ui::has_flag(component.list_5_items_[0].root->dirty(), ui::Dirty::Paint) &&
              !ui::has_flag(component.list_5_items_[0].root->dirty(), ui::Dirty::Layout),
          "a repainted item text is Paint-only (it is the template's text node)");

    // --- 7. Cap: the table truncates deterministically at kMaxGeneratedListItems. --------------
    clear_all(root);
    component.state_items_.clear();
    for (int index = 0; index < 40; ++index) {
        component.state_items_.push_back("item" + std::to_string(index));
    }
    component.apply_state();
    check(component.list_5_count_ == dynamic::gen::kMaxGeneratedListItems,
          "the item table truncates at the documented cap");
    {
        std::set<ui::NodeId> ids;
        for (std::size_t index = 0; index < component.list_5_count_; ++index) {
            ids.insert(component.list_5_items_[index].root->id());
        }
        check(ids.size() == component.list_5_count_, "every truncated item has a unique id");
    }
    check(text_of(*component.list_5_items_[0].root) == "item0 @ 2",
          "the truncated prefix is the source prefix");

    // --- 8. Ids stay unique when a reused item outlives the slot it was first built in. ---------
    component.state_items_ = {"a", "b"};
    component.apply_state();
    component.state_items_ = {"b"};
    component.apply_state();
    component.state_items_ = {"b", "c"};
    component.apply_state();
    check(component.list_5_count_ == 2 &&
              component.list_5_items_[0].root->id() != component.list_5_items_[1].root->id(),
          "a new item never reuses the id of a surviving item");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen dynamic runtime tests passed\n";
    return 0;
}
