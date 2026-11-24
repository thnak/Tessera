// SPDX-License-Identifier: MIT
//
// The dense per-generation style memo (issue #735, slice G). `SceneStyleTree` used to key its
// computed styles by `const Node*` in an `std::unordered_map`; it now keeps one slot per
// `entries_` element, in traversal order, with a parallel "has a style" flag. The behaviours this
// file pins are the ones the hash map gave for free and the array has to earn:
//
//   * `resolve` returns a reference that stays valid until the next `begin_memo` or the tree's
//     destruction - the array is sized once in `build`, so resolving more nodes never moves the
//     ones already handed out;
//   * `begin_memo(false)` clears every memoised style, `begin_memo(true)` keeps them;
//   * `restyle_subtree` records the layout digest it dropped, and `restyled_layout_changed`
//     consumes that record;
//   * a node outside the captured tree resolves to the registry's initial style;
//   * `compute` with a container-size source keeps the chain walk and leaves the memo alone.
//
// The edits below are inline declarations, because `resolve` reads those from the scene node on
// every call while the selector view (class, id, ...) is only re-read by `restyle_subtree`. That
// keeps each test about the memo rather than about the view refresh, which `style_var` covers.
//
// Deterministic: no clock, no GPU, no font.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/container_query.hpp"
#include "tessera/ui/style/layout_digest.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::ContainerSize;
using tessera::ui::style::layout_digest;
using tessera::ui::style::SceneStyleTree;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_style_memo: " << message << "\n";
        ++failures;
    }
}

std::string_view value_of(const ComputedStyle& style, std::string_view property) {
    const std::string* value = style.find(property);
    return value != nullptr ? std::string_view(*value) : std::string_view{};
}

// A three-level scene: root -> card -> label. The sheet gives each level a distinct paint and
// layout value, so a stale slot is visible as a wrong colour or width.
struct Scene {
    std::unique_ptr<Node> root = std::make_unique<Node>(1);
    Node* card = nullptr;
    Node* label = nullptr;

    Scene() {
        root->set_element_kind("View");
        root->set_element_id("app");
        card = &root->add_child(std::make_unique<Node>(2));
        card->set_element_kind("View");
        card->set_class_name("card");
        label = &card->add_child(std::make_unique<Node>(3));
        label->set_element_kind("Text");
    }
};

constexpr const char* kSheet = R"(
#app { color: red; }
.card { background-color: #111111; width: 100px; }
Text { font-size: 14px; }
)";

// The reference `resolve` hands out must survive every later `resolve` in the same generation. The
// hash map gave that for free (node-keyed, stable nodes); the array has to size itself up front.
void test_reference_stability() {
    Scene scene;
    const auto sheet = compile_stylesheet(kSheet);
    SceneStyleTree tree(*scene.root);
    tree.begin_memo();

    const ComputedStyle& root_style = tree.resolve(sheet, *scene.root);
    const ComputedStyle* const root_address = &root_style;
    const std::string root_color(value_of(root_style, "color"));
    // Resolve the rest of the tree: a growing vector would move `root_style` here.
    const ComputedStyle& card_style = tree.resolve(sheet, *scene.card);
    const ComputedStyle& label_style = tree.resolve(sheet, *scene.label);
    check(&tree.resolve(sheet, *scene.root) == root_address,
          "a resolved style keeps its address across later resolves");
    check(value_of(tree.resolve(sheet, *scene.root), "color") == root_color,
          "a resolved style keeps its value across later resolves");
    check(value_of(card_style, "width") == "100px", "the card resolves its own width");
    check(value_of(label_style, "font-size") == "14px", "the label resolves its own font size");
    check(value_of(label_style, "color") == "red", "the label inherits the root colour");
    tree.end_memo();
}

// `begin_memo(false)` drops every slot, so an edit the caller did not restyle is picked up by the
// next generation. `begin_memo(true)` keeps them, so only a `restyle_subtree` recomputes.
void test_keep_entries() {
    Scene scene;
    const auto sheet = compile_stylesheet(kSheet);
    SceneStyleTree tree(*scene.root);

    tree.begin_memo();
    check(value_of(tree.resolve(sheet, *scene.card), "background-color") == "#111111",
          "the card starts with the cascade colour");
    tree.end_memo();

    // An edit with no restyle: the memo still holds the previous generation's answer.
    check(scene.card->set_inline_style("background-color", "green"), "the inline edit is accepted");
    tree.begin_memo(true);
    check(value_of(tree.resolve(sheet, *scene.card), "background-color") == "#111111",
          "keep_entries keeps the previous generation's style");
    tree.end_memo();

    // A new generation clears it, so the same edit is now visible.
    tree.begin_memo();
    check(value_of(tree.resolve(sheet, *scene.card), "background-color") == "green",
          "a fresh generation recomputes the edited style");
    tree.end_memo();

    // And `restyle_subtree` is the incremental path: it drops exactly that subtree.
    check(scene.card->set_inline_style("background-color", "teal"), "the second edit is accepted");
    tree.begin_memo(true);
    check(value_of(tree.resolve(sheet, *scene.card), "background-color") == "green",
          "the kept style is still the previous one before the restyle");
    tree.restyle_subtree(*scene.card);
    check(value_of(tree.resolve(sheet, *scene.card), "background-color") == "teal",
          "restyle_subtree recomputes the subtree it dropped");
    tree.end_memo();
}

// `restyle_subtree` remembers the layout digest it dropped, and `restyled_layout_changed` compares
// it against the style the restyle produced. A paint-only change must not read as a layout change.
void test_restyled_layout_changed() {
    Scene scene;
    const auto sheet = compile_stylesheet(kSheet);
    SceneStyleTree tree(*scene.root);
    tree.begin_memo();
    // The digest is cached with the style's shared entries, so the host's layout signature has to
    // have folded it in once for the "before" value to exist. That is what this call stands in for.
    (void)layout_digest(tree.resolve(sheet, *scene.card));
    // `restyle_subtree` refreshes the whole subtree, and a node with no memoised style is recorded
    // as changed (there is no "before" to compare against), so warm the label too.
    (void)layout_digest(tree.resolve(sheet, *scene.label));
    tree.end_memo();

    // Paint-only: the inline edit moves `background-color` and nothing layout reads.
    tree.clear_restyled();
    tree.begin_memo(true);
    check(scene.card->set_inline_style("background-color", "green"), "the paint edit is accepted");
    tree.restyle_subtree(*scene.card);
    check(!tree.restyled_layout_changed(sheet), "a paint-only restyle keeps the layout digest");
    tree.end_memo();

    // Layout-affecting: the same shape of edit, but the declaration now sets `width`.
    tree.clear_restyled();
    tree.begin_memo(true);
    check(scene.card->set_inline_style("width", "200px"), "the layout edit is accepted");
    tree.restyle_subtree(*scene.card);
    check(tree.restyled_layout_changed(sheet), "a width change moves the layout digest");
    tree.end_memo();

    // The record is consumed: a second answer for the same round has nothing left to compare.
    check(!tree.restyled_layout_changed(sheet),
          "the restyle record is consumed by the first answer");
}

// A node outside the captured tree has no slot, so it resolves to the registry's initial style -
// the same answer the un-memoised `compute` gives.
void test_outside_tree() {
    Scene scene;
    const auto sheet = compile_stylesheet(kSheet);
    SceneStyleTree tree(*scene.root);
    Node stranger(99);
    stranger.set_element_kind("View");
    stranger.set_class_name("card");

    tree.begin_memo();
    const ComputedStyle& outside = tree.resolve(sheet, stranger);
    check(outside.properties() == sheet.registry().initial_style().properties(),
          "a node outside the tree resolves to the registry's initial style");
    check(&tree.resolve(sheet, stranger) == &outside,
          "the outside style is computed once and reused");
    // A fresh generation drops it with the rest, and recomputes the same value.
    tree.begin_memo();
    check(tree.resolve(sheet, stranger).properties() ==
              sheet.registry().initial_style().properties(),
          "a fresh generation recomputes the outside style");
    tree.end_memo();
}

// `compute` with a container-size source keeps the ancestor-chain walk (container queries are not
// memoised), and it must not write the memo: a later `resolve` still answers the plain cascade.
void test_container_source_bypasses_memo() {
    // The plain rule comes first so the `@container` one wins on source order at equal specificity.
    const auto sheet =
        compile_stylesheet(".card { background-color: blue }"
                           "* { container-type: inline-size }"
                           "@container (min-width: 600px) { .card { background-color: red } }");
    Scene scene;
    SceneStyleTree tree(*scene.root);

    struct Sizes {
        const Node* container = nullptr;
        ContainerSize size;
    };
    const auto lookup = [](void* context, const Node& node, ContainerSize& out) {
        const auto* sizes = static_cast<const Sizes*>(context);
        if (&node != sizes->container) {
            return false;
        }
        out = sizes->size;
        return true;
    };
    Sizes sizes{scene.root.get(), ContainerSize{700, 100, false}};

    tree.begin_memo();
    const ComputedStyle& memoised = tree.resolve(sheet, *scene.card);
    check(value_of(memoised, "background-color") == "blue", "resolve answers the plain cascade");
    const ComputedStyle walked = tree.compute(sheet, *scene.card, lookup, &sizes);
    check(value_of(walked, "background-color") == "red",
          "compute with a container source applies the container rule");
    check(value_of(tree.resolve(sheet, *scene.card), "background-color") == "blue",
          "the container walk does not write the memo");
    tree.end_memo();
}

// `resolve` answers a node of a pre-order walk from a cursor (the next slot) before it asks the
// node-keyed index. Whatever order the nodes are asked in, the answer must be the one an in-order
// walk gives: backwards, skipping, repeating, and mixed with a node outside the captured tree.
void test_resolve_order_does_not_change_the_answer() {
    const auto sheet = compile_stylesheet(kSheet);
    const auto snapshot = [&](Scene& scene, const std::vector<Node*>& order) {
        SceneStyleTree tree(*scene.root);
        tree.begin_memo();
        std::vector<std::string> out;
        for (Node* node : order) {
            const ComputedStyle& style = tree.resolve(sheet, *node);
            out.push_back(std::string(value_of(style, "color")) + "|" +
                          std::string(value_of(style, "width")) + "|" +
                          std::string(value_of(style, "font-size")));
        }
        tree.end_memo();
        return out;
    };
    Scene scene;
    Node outside(99);
    const std::vector<std::string> in_order =
        snapshot(scene, {scene.root.get(), scene.card, scene.label});
    // Backwards: the label's parents resolve first through the recursion, then each is a memo hit.
    const std::vector<std::string> backwards =
        snapshot(scene, {scene.label, scene.card, scene.root.get()});
    check(backwards.size() == 3 && backwards[0] == in_order[2] && backwards[1] == in_order[1] &&
              backwards[2] == in_order[0],
          "resolving in reverse gives the in-order answers");
    // Skipping a node, repeating one, and a node that is not in the tree in the middle.
    const std::vector<std::string> mixed =
        snapshot(scene, {scene.root.get(), scene.label, scene.label, &outside, scene.card,
                         scene.root.get(), scene.card});
    check(mixed.size() == 7 && mixed[0] == in_order[0] && mixed[1] == in_order[2] &&
              mixed[2] == in_order[2] && mixed[4] == in_order[1] && mixed[5] == in_order[0] &&
              mixed[6] == in_order[1],
          "skipping, repeating and an outside node do not disturb the answers");
    check(mixed[3] != in_order[0] && mixed[3] != in_order[1] && mixed[3] != in_order[2],
          "a node outside the tree still gets the initial style, not a neighbour's slot");
}

} // namespace

int main() {
    test_resolve_order_does_not_change_the_answer();
    test_reference_stability();
    test_keep_entries();
    test_restyled_layout_changed();
    test_outside_tree();
    test_container_source_bypasses_memo();
    if (failures != 0) {
        std::cerr << "scene_style_memo: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_style_memo: ok\n";
    return 0;
}
