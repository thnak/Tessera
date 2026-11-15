// SPDX-License-Identifier: MIT
//
// The scene -> snapshot walk (issue #166): `ui::automation::snapshot_scene()` is now the one
// implementation of the automation snapshot of docs/specs/automation-ids-v1.md section 5.2, shared
// by the headless harness and the design-time preview. These tests pin the properties both callers
// depend on: preorder emission with parent indices, the derived structural paths, the annotation
// override, hiding, the two caps, and the exact bytes the canonical serializer writes.

#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_value.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/automation/snapshot.hpp"
#include "tessera/ui/automation/snapshot_json.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace ui = tessera::ui;
namespace automation = tessera::ui::automation;

int failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_snapshot: " << message << "\n";
        ++failures;
    }
    return condition;
}

ui::PaintData text_paint(std::string text) {
    ui::PaintData data;
    data.kind = ui::PaintKind::Text;
    data.text = std::move(text);
    return data;
}

// The fixture shape the harness's own scene has: a root, a card with a label and a button, and a
// second child on the root. Two levels, so parent indices and paths are both exercised.
void build_fixture(ui::Node& root) {
    root.set_element_kind("View");
    auto card_owner = std::make_unique<ui::Node>(2);
    ui::Node& card = root.add_child(std::move(card_owner));
    card.set_element_kind("View");
    card.set_automation_id(100);
    card.set_class_name("card");

    auto label_owner = std::make_unique<ui::Node>(3);
    ui::Node& label = card.add_child(std::move(label_owner));
    label.set_element_kind("Text");
    label.set_automation_id(101);
    label.set_paint_data(text_paint("Count: 0"));
    (void)label.set_aria("aria-label", "counter");

    auto button_owner = std::make_unique<ui::Node>(4);
    ui::Node& button = card.add_child(std::move(button_owner));
    button.set_element_kind("Button");
    button.set_automation_id(102);
    button.set_focusable(true);

    auto field_owner = std::make_unique<ui::Node>(5);
    ui::Node& field = root.add_child(std::move(field_owner));
    field.set_element_kind("TextInput");
    field.set_automation_id(103);
}

const automation::UiSnapshotNode* find(const automation::UiSnapshot& snapshot, std::uint64_t id) {
    const auto index = automation::find_by_ui_id(snapshot, id);
    return index == automation::kNoIndex ? nullptr : &snapshot.nodes[index];
}

// Preorder, parent indices and the derived paths: the three things a consumer walks.
void test_tree_shape() {
    ui::Node root(1);
    build_fixture(root);
    const auto snapshot = automation::snapshot_scene(root);
    if (!expect(snapshot.has_value(), "the fixture walk succeeds") ||
        !expect(snapshot->nodes.size() == 5, "the fixture has five nodes")) {
        return;
    }
    expect(snapshot->nodes[0].path == "root", "the root path is `root`");
    expect(snapshot->nodes[0].kind == "View", "the root kind is View");
    expect(snapshot->nodes[0].parent == automation::kNoIndex, "the root has no parent");
    expect(snapshot->nodes[0].depth == 0, "the root is at depth 0");

    const automation::UiSnapshotNode* card = find(*snapshot, 100);
    const automation::UiSnapshotNode* label = find(*snapshot, 101);
    if (!expect(card != nullptr && label != nullptr, "both annotated nodes are present")) {
        return;
    }
    expect(card->path == "root/View[0]", "a child's path is Kind[index] under the parent");
    expect(card->depth == 1, "a child is one level deeper");
    expect(card->parent == 0, "a child indexes its parent");
    expect(label->path == "root/View[0]/Text[0]", "the path nests through both levels");
    expect(label->text == "Count: 0", "a text payload becomes the node's text");
    expect(label->name == "counter", "aria-label becomes the accessible name");
    expect(label->parent == 1, "the grandchild indexes the card");
    // Preorder: a parent is always emitted before its children.
    expect(card->parent < automation::find_by_ui_id(*snapshot, 101),
           "preorder puts the parent before the child");
}

// A hidden node is dropped with its subtree unless `include_hidden` asks for it, and the flag is
// what the harness's `include_hidden` argument maps onto.
void test_hidden() {
    ui::Node root(1);
    root.set_element_kind("View");
    auto hidden_owner = std::make_unique<ui::Node>(2);
    ui::Node& hidden = root.add_child(std::move(hidden_owner));
    hidden.set_element_kind("View");
    hidden.set_automation_id(200);
    hidden.set_style_hidden(true);
    auto child_owner = std::make_unique<ui::Node>(3);
    ui::Node& child = hidden.add_child(std::move(child_owner));
    child.set_element_kind("Text");
    child.set_automation_id(201);

    const auto without = automation::snapshot_scene(root);
    if (expect(without.has_value(), "the hidden walk succeeds")) {
        expect(without->nodes.size() == 1, "a hidden node and its subtree are omitted");
        expect(find(*without, 200) == nullptr, "the hidden node is absent");
    }
    automation::SceneSnapshotOptions options;
    options.snapshot.include_hidden = true;
    const auto with = automation::snapshot_scene(root, options);
    if (expect(with.has_value(), "the include_hidden walk succeeds")) {
        const automation::UiSnapshotNode* node = find(*with, 200);
        if (expect(node != nullptr, "include_hidden keeps the hidden node")) {
            expect(!node->visible, "the kept node reports visible=false");
        }
        const automation::UiSnapshotNode* kept_child = find(*with, 201);
        if (expect(kept_child != nullptr, "include_hidden keeps the subtree")) {
            expect(!kept_child->visible, "a child of a hidden parent is not visible either");
        }
    }
}

// An annotation overrides the derived path and supplies the component id; a node without one keeps
// both of the walk's own answers. This is the seam the harness's generated manifest uses.
struct AnnotationTable {
    std::uint64_t id = 0;
    std::string_view path;
    std::uint64_t component_id = 0;
};

bool annotate(void* raw, std::uint64_t automation_id, automation::SceneAnnotation& out) {
    const auto* table = static_cast<const AnnotationTable*>(raw);
    if (table == nullptr || table->id != automation_id) {
        return false;
    }
    out.path = table->path;
    out.component_id = table->component_id;
    return true;
}

void test_annotation() {
    ui::Node root(1);
    build_fixture(root);
    const AnnotationTable table{100, "Counter/root[0]", 7};
    automation::SceneSnapshotOptions options;
    options.annotate = &annotate;
    options.annotation_context = const_cast<AnnotationTable*>(&table);
    const auto snapshot = automation::snapshot_scene(root, options);
    if (!expect(snapshot.has_value(), "the annotated walk succeeds")) {
        return;
    }
    const automation::UiSnapshotNode* card = find(*snapshot, 100);
    if (expect(card != nullptr, "the annotated node is present")) {
        expect(card->path == "Counter/root[0]", "the annotation replaces the derived path");
        expect(card->component_id == 7, "the annotation supplies the component id");
    }
    const automation::UiSnapshotNode* label = find(*snapshot, 101);
    if (expect(label != nullptr, "an unannotated node is present")) {
        expect(label->component_id == 0, "an unannotated node keeps component id 0");
        expect(label->path == "root/View[0]/Text[0]", "an unannotated node keeps the derived path");
    }
}

// A capped tree fails instead of emitting a partial document: a truncated snapshot would compare
// equal to a truncated snapshot from the other side of a parity gate.
void test_caps() {
    ui::Node root(1);
    build_fixture(root);
    automation::SceneSnapshotOptions shallow;
    shallow.max_depth = 1;
    const auto too_deep = automation::snapshot_scene(root, shallow);
    if (expect(!too_deep.has_value(), "a tree deeper than max_depth fails")) {
        expect(too_deep.error().code == automation::SceneSnapshotErrorCode::TooDeep,
               "the failure names the depth cap");
        expect(too_deep.error().limit == 1, "the failure reports the cap");
        expect(too_deep.error().actual == 2, "the failure reports the offending depth");
    }
    automation::SceneSnapshotOptions narrow;
    narrow.max_nodes = 3;
    const auto too_many = automation::snapshot_scene(root, narrow);
    if (expect(!too_many.has_value(), "a tree larger than max_nodes fails")) {
        expect(too_many.error().code == automation::SceneSnapshotErrorCode::TooManyNodes,
               "the failure names the node cap");
        expect(too_many.error().actual == 4, "the failure reports the count that exceeded the cap");
    }
    // A zero cap means "unset": it falls back to the documented default rather than rejecting every
    // tree, which is what a default-constructed options struct with an explicit 0 would otherwise
    // do.
    automation::SceneSnapshotOptions zero;
    zero.max_nodes = 0;
    zero.max_depth = 0;
    expect(automation::snapshot_scene(root, zero).has_value(), "a zero cap uses the default");
}

// The document is the canonical one: one line, trailing newline, decimal-string ids, `parent` null
// for the root. It has to stay byte-stable because the parity gate compares bytes.
void test_document_bytes() {
    ui::Node root(1);
    build_fixture(root);
    const auto snapshot = automation::snapshot_scene(root);
    if (!expect(snapshot.has_value(), "the document walk succeeds")) {
        return;
    }
    const std::string json = automation::snapshot_to_json(*snapshot);
    expect(!json.empty() && json.back() == '\n', "the document ends with a newline");
    expect(json.find('\n') == json.size() - 1, "the document is one line");
    expect(json.rfind("{\"snapshotVersion\":1,\"sequence\":\"0\",\"nodes\":[", 0) == 0,
           "the header is the documented shape in the documented order");
    // Round-trips through the harness decoder, which is how the harness embeds it, so the two sides
    // of the parity gate are looking at the same document shape.
    auto parsed = tessera::harness::parse_json(json);
    if (expect(parsed.has_value(), "the document decodes as JSON")) {
        const auto* nodes = parsed->find("nodes");
        expect(nodes != nullptr && nodes->as_array() != nullptr, "the document has a nodes array");
        expect(nodes != nullptr && nodes->as_array() != nullptr && nodes->as_array()->size() == 5,
               "every fixture node is emitted");
    }
}

} // namespace

int main() {
    test_tree_shape();
    test_hidden();
    test_annotation();
    test_caps();
    test_document_bytes();
    if (failures != 0) {
        std::cerr << "scene_snapshot: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_snapshot: ok\n";
    return 0;
}
