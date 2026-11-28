// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/node_fwd.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace tessera::ui::a11y {

// Accessibility semantics snapshot (docs/specs/accessibility-v1.md section 1). The builder turns
// the scene graph plus node metadata (role/aria/id/automationId), computed style, focus, and scroll
// state into one flat, pre-order snapshot that a platform bridge can consume without touching the
// scene. This slice never calls a platform API and never invokes an action.
//
// Determinism: the same scene, style, focus, and options produce the same snapshot byte-for-byte.
// Bounds are world bounds in window logical pixels. Ordering is the pre-order traversal of the
// emitted nodes (parent first, then children in child order). The snapshot is bounded: at most
// kMaxNodes entries and kMaxChildren children per entry, with truncation reported in BuildStats.
inline constexpr std::size_t kMaxNodes = 256;
inline constexpr std::size_t kMaxChildren = 64;
// Cap for one stored name/value/description in bytes (UTF-8, truncated at a code-point boundary,
// NUL-terminated inside the array).
inline constexpr std::size_t kMaxTextBytes = 128;

using A11yIndex = std::uint16_t;
inline constexpr A11yIndex kInvalidIndex = std::numeric_limits<A11yIndex>::max();

enum class Role : std::uint8_t {
    Generic,
    Presentation, // decorative: the node itself is not emitted, its children are still visited
    Button,
    Link,
    Text,
    Heading,
    Image,
    TextField,
    Checkbox,
    Radio,
    Switch,
    Slider,
    Progress,
    List,
    ListItem,
    Tab,
    TabPanel,
    Dialog,
    Menu,
    MenuItem,
    ScrollView,
    Tooltip,
    Alert,
    Paragraph, // a block of text (`p`); the user-agent sheet gives it a margin
    Status,    // a polite live region such as the LiveCapsule; announced by its name
    // Not a role: the count of real values, so the role trait table can be sized and checked at
    // compile time (src/ui/accessibility/a11y_role_traits.hpp).
    kCount,
};

// Stable lowercase names (used by tests, diagnostics, and bridge mapping tables).
[[nodiscard]] std::string_view to_string(Role role);
// Maps an ARIA role name (or a schema `impliedRole` spelling such as "scroll view" or "textbox")
// case-insensitively. Unknown names return Role::Generic.
[[nodiscard]] Role role_from_string(std::string_view name);

enum class State : std::uint32_t {
    None = 0,
    Focusable = 1U << 0,
    Focused = 1U << 1,
    Disabled = 1U << 2,
    Selected = 1U << 3,
    Checked = 1U << 4,
    Expanded = 1U << 5,
};

constexpr State operator|(State lhs, State rhs) {
    return static_cast<State>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}
constexpr State operator&(State lhs, State rhs) {
    return static_cast<State>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}
constexpr bool has_state(State value, State flag) {
    return (value & flag) != State::None;
}

enum class Action : std::uint32_t {
    None = 0,
    Focus = 1U << 0,
    Press = 1U << 1,
    SetValue = 1U << 2,
    Scroll = 1U << 3,
};

constexpr Action operator|(Action lhs, Action rhs) {
    return static_cast<Action>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}
constexpr Action operator&(Action lhs, Action rhs) {
    return static_cast<Action>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}
constexpr bool has_action(Action value, Action flag) {
    return (value & flag) != Action::None;
}

// One semantics node. Text fields are inline bounded copies so a snapshot owns no scene strings and
// stays valid after scene mutations; `A11yNode` is trivially copyable apart from nothing (arrays
// only), so a snapshot can be copied or shipped to a bridge thread.
// Where one node sits in a collection, the way ARIA's `aria-setsize`/`aria-posinset` and Android's
// `CollectionItemInfo` describe it (docs/specs/accessibility-v1.md section 5).
//
// **Why it is on the node and not a second table.** A screen reader asks a node "how many items are
// in your list, and which one are you" while it walks that node; a side table keyed by id would be
// a second thing to keep consistent with the snapshot, and the snapshot is already the answer to
// everything else. `set_size == 0` means "not a collection item", so a node outside a collection
// carries the default and costs nothing.
struct CollectionPosition {
    // The collection's item count (`aria-setsize`, Android `rowCount`/`columnCount`).
    std::size_t set_size = 0;
    // This item's 1-based position (`aria-posinset`), matching `virtualization::kPosInSetBase`.
    std::size_t pos_in_set = 1;
};

struct A11yNode {
    NodeId id = 0;
    std::uint64_t automation_id = 0;
    Role role = Role::Generic;
    State states = State::None;
    Action actions = Action::None;
    // Set for a node that is one item of a collection; the default is "not one".
    CollectionPosition collection{};
    std::array<char, kMaxTextBytes> name{};
    std::array<char, kMaxTextBytes> value{};
    std::array<char, kMaxTextBytes> description{};
    // World bounds in logical pixels (the scene's world_bounds AABB).
    rendering::rhi::Rect bounds{};
    std::array<A11yIndex, kMaxChildren> children{};
    std::uint16_t child_count = 0;
    bool children_truncated = false;
};

// Stable views over the inline bounded text.
[[nodiscard]] std::string_view name_of(const A11yNode& node);
[[nodiscard]] std::string_view value_of(const A11yNode& node);
[[nodiscard]] std::string_view description_of(const A11yNode& node);

// A reusable snapshot buffer. `clear` keeps the capacity, so a steady-state rebuild allocates
// nothing after the first pass: call `reserve()` (or build once) before the frame loop.
struct A11ySnapshot {
    std::vector<A11yNode> nodes;

    void clear() {
        nodes.clear();
    }
    [[nodiscard]] std::size_t size() const {
        return nodes.size();
    }
};

// Resolves the computed style of a scene node. Same shape as paint::StyleProvider but declared
// here so the accessibility layer does not depend on the paint/render libraries; a paint pass
// provider can be adapted with a one-line forwarding wrapper. `compute` must not mutate the scene.
class StyleProvider {
public:
    virtual ~StyleProvider() = default;
    [[nodiscard]] virtual style::ComputedStyle compute(const Node& node) = 0;
};

// Answers "where does this node sit in its collection", for the builder (docs/specs/
// accessibility-v1.md section 5).
//
// It is a callback rather than a container so the caller can keep the collection's own source of
// truth - `virtualization::CollectionSemantics` answers from its key index, which is the same
// object the range is driven by. Returning `std::nullopt` means "not a collection item", so a
// builder configured with a source that recognises nothing produces exactly the snapshot it did
// before this existed.
using CollectionPositionSource = std::function<std::optional<CollectionPosition>(NodeId)>;

struct BuildOptions {
    // The FocusManager's current focus (`0` = none). Only this node can carry State::Focused, and
    // it is the caller's source of truth: the builder never derives focus from pseudo-state bits.
    NodeId focused_id = 0;
    // Hard cap for this snapshot; clamped to kMaxNodes.
    std::size_t max_nodes = kMaxNodes;
    // Where a node sits in a collection. Empty means no node is a collection item, which is what a
    // caller without a virtualized list wants.
    CollectionPositionSource collection_position;
};

// Counters for one build. "skipped_*" count skipped subtrees/nodes exactly as documented in
// docs/specs/accessibility-v1.md section 12.
struct BuildStats {
    std::size_t visited = 0;              // scene nodes examined
    std::size_t emitted = 0;              // snapshot entries produced
    std::size_t skipped_invisible = 0;    // Node::visible() == false (subtree)
    std::size_t skipped_aria_hidden = 0;  // aria-hidden="true" (subtree)
    std::size_t skipped_inert = 0;        // before a visible aria-modal="true" node (subtree)
    std::size_t skipped_style_hidden = 0; // display:none or visibility:hidden|collapse (subtree)
    std::size_t skipped_clipped = 0;      // outside every ancestor scrollport (subtree or node)
    std::size_t skipped_zero_size = 0;    // zero-area node; children still visited
    std::size_t skipped_decorative = 0;   // presentation/none role; children still visited
    std::size_t dropped_nodes = 0;        // nodes not emitted because the cap was reached
    std::size_t truncated_children = 0;   // nodes whose child list hit kMaxChildren
    std::size_t truncated_text = 0;       // name/value/description strings truncated
    bool truncated = false;               // the node cap was reached (traversal stopped)
};

// Builds the semantics snapshot of `root` into `out` (cleared first, capacity reused).
// The scene is only read. Every emitted node records its children's indices into `out.nodes`.
[[nodiscard]] BuildStats build_a11y_tree(Node& root, StyleProvider& styles, A11ySnapshot& out,
                                         const BuildOptions& options = {});

// First pre-order entry matching `automation_id`/`node id`; kInvalidIndex when absent.
[[nodiscard]] A11yIndex find_by_automation_id(const A11ySnapshot& snapshot,
                                              std::uint64_t automation_id);
[[nodiscard]] A11yIndex find_by_node_id(const A11ySnapshot& snapshot, NodeId id);

} // namespace tessera::ui::a11y
