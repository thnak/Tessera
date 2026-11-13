// SPDX-License-Identifier: MIT

// Runtime proof of the conditional + keyed-list codegen contract (docs/design/compiler.md section
// 11): the generated component builds every conditional branch once and attaches/detaches it at its
// deterministic source position, and the bounded list item table reuses nodes by key, reorders
// in place, grows past 32 items without truncating, and marks nothing dirty when nothing changed.
//
// The component under test is the developer class `dynamic::Dynamic` over the committed golden base
// `tests/golden/codegen/Dynamic.g.hpp`, generated from the hand-built IR in
// codegen_dynamic_tests.cpp (the `.tsx` front end cannot author a list type yet). State is driven
// through the generated setters and observed through the scene, never through generated fields.

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
    dynamic::Dynamic component;
    component.set_items({"beta", "alpha"});
    ui::Node scene(1000);
    component.build(scene);

    check(scene.children().size() == 1, "the component builds exactly one root child");
    ui::Node& root = *scene.children()[0];

    // --- 1. Initial child set: static children + list items, conditional branches detached.
    // -------
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
    check(root.children().size() == 7 && root.children()[2]->id() == 5 &&
              root.children()[3]->id() == 18,
          "item node ids are the template id plus the item index times kItemIdStride");

    // --- 2. Toggle: the branch attaches at its source position, the ternary swaps. --------------
    clear_all(root);
    component.set_count(1);
    component.apply_state();
    check(root.children().size() == 8, "the toggled child set has one more child");
    if (root.children().size() == 8) {
        check_eq(joined(root), "Header|Positive|Count: 1|beta @ 1|alpha @ 1|||Footer",
                 "the attached branch sits at its source position and the ternary swapped");
        check_eq(root.children()[1]->element_kind(), "Text", "the attached branch is the Text");
        check_eq(root.children()[5]->children()[0]->paint_data()->text, "Filled",
                 "the complementary ternary branch is now present");
    }
    check(ui::has_flag(root.dirty(), ui::Dirty::Layout) &&
              ui::has_flag(root.dirty(), ui::Dirty::Paint) &&
              ui::has_flag(root.dirty(), ui::Dirty::Render),
          "a child-set change marks Layout|Paint|Render on the shared parent");
    check(ui::has_flag(root.children()[1]->dirty(), ui::Dirty::Layout) &&
              ui::has_flag(root.children()[1]->dirty(), ui::Dirty::Paint),
          "an attached branch is marked dirty in the layout and paint domains");
    check(!ui::has_flag(root.dirty(), ui::Dirty::Style),
          "a child-set change never marks the selector domain");

    // --- 3. No-change update: nothing dirty, same table, same hashes. ---------------------------
    clear_all(root);
    ui::Node* const first_item = root.children()[3].get();
    component.apply_state();
    check(true_at_dirty(root), "an unchanged update marks no dirty domain on the root");
    check(root.children()[3].get() == first_item, "an unchanged key list keeps its item nodes");
    check(root.children().size() == 8, "an unchanged update does not rebuild children");

    // --- 4. Reorder: nodes are reused by key, only the positions change. ------------------------
    ui::Node* const beta = root.children()[3].get();
    ui::Node* const alpha = root.children()[4].get();
    clear_all(root);
    component.set_items({"alpha", "beta"});
    component.apply_state();
    check(root.children()[3].get() == alpha && root.children()[4].get() == beta,
          "a reorder reuses the item nodes by key");
    check_eq(joined(root), "Header|Positive|Count: 1|alpha @ 1|beta @ 1|||Footer",
             "the reordered items paint in the source order");
    check(text_of(*alpha) == "alpha @ 1" && text_of(*beta) == "beta @ 1",
          "reused item content is unchanged by the reorder");

    // --- 5. Remove and insert: stale nodes leave, new keys build one node each. ----------------
    clear_all(root);
    component.set_items({"alpha"});
    component.apply_state();
    check(root.children().size() == 7, "removing a key drops one item");
    check_eq(joined(root), "Header|Positive|Count: 1|alpha @ 1|||Footer",
             "the removed item leaves no child behind");
    clear_all(root);
    component.set_items({"alpha", "gamma", "beta"});
    component.apply_state();
    check(root.children().size() == 9, "inserting keys grows the child set");
    check(root.children()[3].get() == alpha, "the surviving key keeps its node across an insert");
    check(root.children()[3] != root.children()[4] && root.children()[4] != root.children()[5],
          "each key owns a distinct item node");
    check_eq(joined(root), "Header|Positive|Count: 1|alpha @ 1|gamma @ 1|beta @ 1|||Footer",
             "the inserted key is materialized in order");

    // --- 6. Content-only change: same keys, new text, no structural work. ----------------------
    clear_all(root);
    component.set_count(2);
    component.apply_state();
    check(root.children()[3].get() == alpha, "a content-only change keeps the item nodes");
    check_eq(joined(root), "Header|Positive|Count: 2|alpha @ 2|gamma @ 2|beta @ 2|||Footer",
             "the per-item content pass repainted every item text");
    check(root.dirty() == ui::Dirty::Clean,
          "a content-only item change marks only the item text, not the parent");
    check(ui::has_flag(alpha->dirty(), ui::Dirty::Paint) &&
              !ui::has_flag(alpha->dirty(), ui::Dirty::Layout),
          "a repainted item text is Paint-only (it is the template's text node)");

    // --- 7. No cap: a list longer than 32 items is fully materialized (issue #4). ---------------
    clear_all(root);
    std::vector<std::string> many;
    for (int index = 0; index < 40; ++index) {
        many.push_back("item" + std::to_string(index));
    }
    component.set_items(many);
    component.apply_state();
    // Header, Positive, Count, 40 items, the two ternary views, Footer.
    check(root.children().size() == 46, "a 40-item source is not truncated");
    {
        std::set<ui::NodeId> ids;
        for (std::size_t index = 3; index < 43; ++index) {
            ids.insert(root.children()[index]->id());
        }
        check(ids.size() == 40, "every item of a long list has a unique id");
    }
    check(text_of(*root.children()[3]) == "item0 @ 2", "the first item is the source head");
    check(text_of(*root.children()[42]) == "item39 @ 2", "the last source item is materialized");

    // --- 8. Ids stay unique when a reused item outlives the slot it was first built in. ---------
    component.set_items({"a", "b"});
    component.apply_state();
    component.set_items({"b"});
    component.apply_state();
    component.set_items({"b", "c"});
    component.apply_state();
    check(root.children().size() == 8 && root.children()[3]->id() != root.children()[4]->id(),
          "a new item never reuses the id of a surviving item");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen dynamic runtime tests passed\n";
    return 0;
}
