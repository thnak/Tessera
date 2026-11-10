// SPDX-License-Identifier: MIT

#include "tessera/ui/automation/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::automation::build_snapshot;
using tessera::ui::automation::find_by_ui_id;
using tessera::ui::automation::kNoIndex;
using tessera::ui::automation::kSnapshotVersion;
using tessera::ui::automation::SnapshotBounds;
using tessera::ui::automation::SnapshotOptions;
using tessera::ui::automation::SnapshotSourceNode;
using tessera::ui::automation::UiId;
using tessera::ui::automation::UiSnapshot;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "automation_snapshot: " << message << "\n";
    }
    return condition;
}

SnapshotSourceNode make_node(UiId ui_id, std::string_view kind, std::string_view path) {
    SnapshotSourceNode node;
    node.ui_id = ui_id;
    node.component_id = 900;
    node.kind = kind;
    node.path = path;
    return node;
}

// root
//   Button[1]
//   List[2]
//     item (alpha)
//     item (beta)
//   Panel[3] (hidden)
//     Label[0]
SnapshotSourceNode make_tree() {
    SnapshotSourceNode root = make_node(100, "View", "Counter/root[0]");
    root.name = "Counter";
    root.bounds = SnapshotBounds{1.0f, 2.0f, 300.0f, 200.0f};

    SnapshotSourceNode button = make_node(10, "Button", "Counter/root[0]/Button[1]");
    button.text = "Go";
    button.focused = true;

    SnapshotSourceNode list = make_node(20, "List", "Counter/root[0]/List[2]");
    SnapshotSourceNode first = make_node(30, "ListItem", "Counter/root[0]/List[2]");
    first.item_key = "alpha";
    first.text = "Alpha";
    SnapshotSourceNode second = make_node(30, "ListItem", "Counter/root[0]/List[2]");
    second.item_key = "beta";
    second.text = "Beta";
    list.children.push_back(first);
    list.children.push_back(second);

    SnapshotSourceNode hidden = make_node(40, "Panel", "Counter/root[0]/Panel[3]");
    hidden.visible = false;
    SnapshotSourceNode hidden_child = make_node(41, "Label", "Counter/root[0]/Panel[3]/Label[0]");
    hidden.children.push_back(hidden_child);

    root.children.push_back(button);
    root.children.push_back(list);
    root.children.push_back(hidden);
    return root;
}

} // namespace

int main() {
    const SnapshotSourceNode tree = make_tree();
    const SnapshotOptions options{7, false};
    const UiSnapshot snapshot = build_snapshot(tree, options);

    // Preorder == document order: parent first, children in source order; the hidden Panel and its
    // subtree are omitted.
    if (!expect(snapshot.version == kSnapshotVersion, "snapshot version") ||
        !expect(snapshot.sequence == 7, "sequence copied") ||
        !expect(snapshot.nodes.size() == 5, "hidden subtree omitted") ||
        !expect(snapshot.nodes[0].ui_id == 100 && snapshot.nodes[0].parent == kNoIndex &&
                    snapshot.nodes[0].depth == 0,
                "root entry") ||
        !expect(snapshot.nodes[1].ui_id == 10 && snapshot.nodes[1].parent == 0 &&
                    snapshot.nodes[1].depth == 1,
                "button entry") ||
        !expect(snapshot.nodes[2].ui_id == 20 && snapshot.nodes[2].parent == 0, "list entry") ||
        !expect(snapshot.nodes[3].ui_id == 30 && snapshot.nodes[3].parent == 2 &&
                    snapshot.nodes[3].depth == 2,
                "first item entry") ||
        !expect(snapshot.nodes[4].ui_id == 30 && snapshot.nodes[4].parent == 2,
                "second item entry")) {
        return 1;
    }

    // Fields are copied, including (UiId, ItemKey) list identity and bounds.
    if (!expect(snapshot.nodes[0].bounds == SnapshotBounds{1.0f, 2.0f, 300.0f, 200.0f},
                "bounds copied") ||
        !expect(snapshot.nodes[0].kind == "View" && snapshot.nodes[0].name == "Counter",
                "kind and name copied") ||
        !expect(snapshot.nodes[1].text == "Go" && snapshot.nodes[1].focused, "text/focus copied") ||
        !expect(snapshot.nodes[3].item_key == "alpha" && snapshot.nodes[4].item_key == "beta",
                "list item keys copied") ||
        !expect(snapshot.nodes[0].component_id == 900, "component id copied") ||
        !expect(snapshot.nodes[0].visible && snapshot.nodes[1].enabled, "visibility defaults")) {
        return 2;
    }

    // List items keep one UiId; the ItemKey disambiguates.
    if (!expect(find_by_ui_id(snapshot, 30) == 3, "first item by ui id") ||
        !expect(find_by_ui_id(snapshot, 30, "beta") == 4, "item by ui id and key") ||
        !expect(find_by_ui_id(snapshot, 30, "gamma") == kNoIndex, "unknown item key") ||
        !expect(find_by_ui_id(snapshot, 41) == kNoIndex, "hidden node not found") ||
        !expect(find_by_ui_id(snapshot, 20, "") == 2,
                "an empty key matches the unkeyed list node") ||
        !expect(find_by_ui_id(snapshot, 20) == 2, "list itself is found without a key")) {
        return 3;
    }

    // include_hidden keeps hidden/offscreen nodes (flagged visible=false) and their subtrees.
    const UiSnapshot with_hidden = build_snapshot(tree, SnapshotOptions{8, true});
    if (!expect(with_hidden.nodes.size() == 7, "include_hidden keeps the subtree") ||
        !expect(with_hidden.nodes[5].ui_id == 40 && with_hidden.nodes[5].parent == 0 &&
                    !with_hidden.nodes[5].visible,
                "hidden node entry") ||
        !expect(with_hidden.nodes[6].ui_id == 41 && with_hidden.nodes[6].parent == 5 &&
                    with_hidden.nodes[6].depth == 2,
                "hidden child entry") ||
        !expect(find_by_ui_id(with_hidden, 41) == 6, "hidden child found when included")) {
        return 4;
    }

    // A hidden root yields an empty snapshot by default; the forest overload visits roots in order
    // and gives every root no parent.
    SnapshotSourceNode hidden_root = make_node(50, "Panel", "Counter/hidden");
    hidden_root.visible = false;
    if (!expect(build_snapshot(hidden_root).nodes.empty(), "hidden root omitted") ||
        !expect(build_snapshot(hidden_root, SnapshotOptions{0, true}).nodes.size() == 1,
                "hidden root included")) {
        return 5;
    }

    const std::array<SnapshotSourceNode, 2> forest{make_node(1, "View", "a"),
                                                   make_node(2, "View", "b")};
    const UiSnapshot forest_snapshot = build_snapshot(std::span<const SnapshotSourceNode>(forest));
    if (!expect(forest_snapshot.nodes.size() == 2, "forest size") ||
        !expect(forest_snapshot.nodes[0].parent == kNoIndex &&
                    forest_snapshot.nodes[1].parent == kNoIndex,
                "forest roots have no parent") ||
        !expect(forest_snapshot.nodes[0].ui_id == 1 && forest_snapshot.nodes[1].ui_id == 2,
                "forest document order")) {
        return 6;
    }

    // Empty input is well-formed, and repeated builds are byte-for-byte identical.
    const std::span<const SnapshotSourceNode> no_roots;
    if (!expect(build_snapshot(no_roots).nodes.empty(), "empty forest") ||
        !expect(build_snapshot(tree, options) == snapshot, "repeated build equal") ||
        !expect(build_snapshot(tree, SnapshotOptions{7, true}) != snapshot,
                "options change the snapshot")) {
        return 7;
    }

    return 0;
}
