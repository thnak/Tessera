// SPDX-License-Identifier: MIT

#include "tessera/ui/automation/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::automation {
namespace {

void append_subtree(const SnapshotSourceNode& node, std::size_t parent, std::uint32_t depth,
                    bool include_hidden, UiSnapshot& out) {
    if (!node.visible && !include_hidden) {
        return;
    }

    UiSnapshotNode entry;
    entry.ui_id = node.ui_id;
    entry.component_id = node.component_id;
    entry.parent = parent;
    entry.depth = depth;
    entry.item_key = node.item_key;
    entry.kind = node.kind;
    entry.path = node.path;
    entry.name = node.name;
    entry.text = node.text;
    entry.bounds = node.bounds;
    entry.visible = node.visible;
    entry.enabled = node.enabled;
    entry.focused = node.focused;
    entry.selected = node.selected;
    const std::size_t index = out.nodes.size();
    out.nodes.push_back(std::move(entry));

    for (const SnapshotSourceNode& child : node.children) {
        append_subtree(child, index, depth + 1, include_hidden, out);
    }
}

} // namespace

UiSnapshot build_snapshot(const SnapshotSourceNode& root, const SnapshotOptions& options) {
    return build_snapshot(std::span<const SnapshotSourceNode>(&root, 1), options);
}

UiSnapshot build_snapshot(std::span<const SnapshotSourceNode> roots,
                          const SnapshotOptions& options) {
    UiSnapshot snapshot;
    snapshot.sequence = options.sequence;
    snapshot.nodes.reserve(roots.size());
    for (const SnapshotSourceNode& root : roots) {
        append_subtree(root, kNoIndex, 0, options.include_hidden, snapshot);
    }
    return snapshot;
}

std::size_t find_by_ui_id(const UiSnapshot& snapshot, UiId ui_id,
                          std::optional<std::string_view> item_key) {
    for (std::size_t index = 0; index < snapshot.nodes.size(); ++index) {
        const UiSnapshotNode& node = snapshot.nodes[index];
        if (node.ui_id != ui_id) {
            continue;
        }
        if (item_key.has_value() && node.item_key != *item_key) {
            continue;
        }
        return index;
    }
    return kNoIndex;
}

} // namespace tessera::ui::automation
