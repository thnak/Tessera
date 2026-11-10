// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/automation/id.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::automation {

// Deterministic automation snapshot (docs/specs/automation-ids-v1.md sections 4-5). The input is a
// plain synthetic node tree so the compiler, codegen fixtures and the runtime can all feed the
// same builder; it never depends on the scene graph, the rendering types or the compiler's JSON.
inline constexpr std::uint32_t kSnapshotVersion = 1;
inline constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

// Logical-pixel bounds. Kept as floats like the scene, but serialized with fixed precision so the
// JSON stays byte-identical across platforms.
struct SnapshotBounds {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    friend constexpr bool operator==(const SnapshotBounds&, const SnapshotBounds&) = default;
};

// One input node. `key` style list/grid identity is `item_key`: a list item keeps the same `ui_id`
// for every instance and is disambiguated by `(ui_id, item_key)` (section 1).
struct SnapshotSourceNode {
    UiId ui_id = 0;
    ComponentId component_id = 0;
    std::string kind;     // debug kind, e.g. "Button"
    std::string path;     // stable structural path, e.g. "Counter/root[0]/Button[1]"
    std::string item_key; // list/grid instance identity; empty when not a list item
    std::string name;     // accessible name
    std::string text;     // text content, optional
    SnapshotBounds bounds{};
    // A hidden/offscreen node is omitted together with its subtree unless include_hidden is set:
    // a container that is not on screen does not expose visible children either.
    bool visible = true;
    bool enabled = true;
    bool focused = false;
    bool selected = false;
    std::vector<SnapshotSourceNode> children;
};

// One emitted snapshot entry. `parent` indexes `UiSnapshot::nodes` in preorder (kNoIndex for a
// root); `depth` is its nesting level. Entries are owned copies, so a snapshot stays valid after
// the source tree changes.
struct UiSnapshotNode {
    UiId ui_id = 0;
    ComponentId component_id = 0;
    std::size_t parent = kNoIndex;
    std::uint32_t depth = 0;
    std::string item_key;
    std::string kind;
    std::string path;
    std::string name;
    std::string text;
    SnapshotBounds bounds{};
    bool visible = true;
    bool enabled = true;
    bool focused = false;
    bool selected = false;

    friend bool operator==(const UiSnapshotNode&, const UiSnapshotNode&) = default;
};

struct SnapshotOptions {
    // Written into the snapshot header as-is; the caller owns the sequence counter.
    std::uint64_t sequence = 0;
    // Include hidden/offscreen nodes (marked visible=false) instead of omitting them.
    bool include_hidden = false;
};

struct UiSnapshot {
    std::uint32_t version = kSnapshotVersion;
    std::uint64_t sequence = 0;
    std::vector<UiSnapshotNode> nodes; // tree preorder == document order

    friend bool operator==(const UiSnapshot&, const UiSnapshot&) = default;
};

// Builds the snapshot in preorder (document order), parent before children, children in source
// order. The same input tree and options always produce the same node list.
[[nodiscard]] UiSnapshot build_snapshot(const SnapshotSourceNode& root,
                                        const SnapshotOptions& options = {});
// Forest overload: roots are visited in the given order.
[[nodiscard]] UiSnapshot build_snapshot(std::span<const SnapshotSourceNode> roots,
                                        const SnapshotOptions& options = {});

// First preorder entry matching `ui_id`; `item_key` (when set) must match exactly, which is how a
// repeated list-item id resolves to one instance. kNoIndex when absent.
[[nodiscard]] std::size_t find_by_ui_id(const UiSnapshot& snapshot, UiId ui_id,
                                        std::optional<std::string_view> item_key = std::nullopt);

} // namespace tessera::ui::automation
