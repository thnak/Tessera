// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string_view>

namespace tessera::ui::a11y {

// At most this many queued action requests. A flooding client drops its own oldest request; the
// queue never grows and the UI thread's drain is bounded by it.
inline constexpr std::size_t kMaxProviderActions = 32;
// At most this many children answered for one node. Matches the snapshot's own per-node cap, so an
// answer can never be longer than the tree that produced it.
inline constexpr std::size_t kMaxProviderChildren = kMaxChildren;

// The virtual id of the host view (the window itself). Real nodes start at 1, so 0 is never a
// node and a caller can test it without a sentinel constant.
inline constexpr int kHostVirtualId = 0;

// One node's answer for the platform. Values are views into the served snapshot, which the provider
// keeps alive until the next publish; the platform must copy them out before that. A caller that
// holds an answer across a publish is holding a dangling view, so every entry point copies into the
// platform's own object.
struct ProviderAnswer {
    // The node's role, so a platform bridge maps it to its own vocabulary.
    Role role = Role::Generic;
    std::string_view text;
    std::string_view description;
    std::uint64_t node_id = 0;
    // World bounds in logical pixels. A screen reader draws its focus rectangle from these and
    // scrolls them into view, so they are part of the answer rather than a separate query.
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    bool focusable = false;
    bool enabled = true;
    bool selected = false;
    bool checkable = false;
    bool checked = false;
    bool editable = false;
    bool scrollable = false;
    bool focused = false;
    // The engine action set, so the caller maps it with `android_a11y_mapping`'s own rules instead
    // of this header duplicating the table.
    Action actions = Action::None;
};

// One action request from the platform, resolved to a scene node and an engine action.
//
// `text` is the payload `Action::SetValue` carries ("replace this field's text"). A request without
// one is a request the host cannot honour, which is why Android's `SetText` used to be offered and
// then dropped. The buffer is bounded and NUL-terminated, the same shape UIA's `UiaActionRequest`
// uses for `ValuePattern.SetValue`, so both platforms carry the same information rather than each
// inventing a subset.
//
// There is deliberately no selection payload: the engine's `Action` has no `SetSelection` member,
// so a caret request is not something this bridge can express. Android's `ACTION_SET_SELECTION` is
// therefore refused rather than accepted and ignored.
inline constexpr std::size_t kMaxProviderActionTextBytes = 1024;

struct ProviderAction {
    ui::NodeId node = 0;
    Action action = Action::None;
    // The text a `SetValue` request carries; empty for every other action.
    std::array<char, kMaxProviderActionTextBytes> text{};

    // The payload as a view, which is how a consumer reads it: the array is fixed-size, so the
    // string ends at the first NUL rather than at the array's end.
    [[nodiscard]] std::string_view text_of() const noexcept {
        return std::string_view(text.data(), ::strnlen(text.data(), text.size()));
    }
};

// A synchronous, queryable view of the last published semantics snapshot (issue #189,
// docs/design/mobile-integration.md section 3.4).
//
// The two existing bridges are pushes: UIA and AT-SPI call out on their own thread and the app only
// answers. Android is the opposite shape. `AccessibilityNodeProvider` is a *pull* interface: the
// platform asks "describe virtual view 7" and expects the answer synchronously, on the Java thread,
// while the engine keeps rendering on its own thread. This type is that pull side.
//
// Virtual ids are stable. A snapshot index is a pre-order position and renumbers on every rebuild,
// which a screen reader would read as "the button I was focused on is now a different button". The
// provider therefore hands out small integers from a slot table and keeps a scene node on the same
// slot for as long as it stays in the tree.
//
// Threading (ADR-021): `publish` and `take_actions` run on the UI thread; every other method may
// run on any thread. The mutex covers one bounded copy of at most kMaxNodes entries, never a
// snapshot build and never platform I/O, so the UI thread never waits on the platform and the
// platform never waits on a frame.
class SnapshotProvider {
public:
    SnapshotProvider();

    // UI thread: replaces the served snapshot. Reuses the retained buffer, so a steady-state
    // publish allocates nothing, and keeps already-assigned virtual ids for nodes that are still
    // present.
    void publish(const A11ySnapshot& snapshot);

    // Any thread: the number of addressable nodes (the host view is not counted).
    [[nodiscard]] std::size_t count() const;
    // Any thread: true once a snapshot has been published, so the platform can answer "empty tree"
    // honestly instead of "no answer yet".
    [[nodiscard]] bool has_snapshot() const;
    // Any thread: the virtual id carrying State::Focused, or `kHostVirtualId` when none does.
    [[nodiscard]] int focused_virtual_id() const;
    // The scene node id behind a virtual id, or 0 when the id is unknown or stale.
    [[nodiscard]] ui::NodeId node_of(int virtual_id) const;

    // Any thread: copies one node out. Returns false for an unknown or stale virtual id, which is a
    // normal race (the platform can query an id from a tree the app already replaced) and must
    // never be an error.
    [[nodiscard]] bool describe(int virtual_id, ProviderAnswer& out) const;
    // Any thread: appends the virtual ids of `virtual_id`'s addressable children. `kHostVirtualId`
    // answers the snapshot's roots: the nodes whose parent is not itself addressable.
    [[nodiscard]] std::size_t children_of(int virtual_id, std::span<int> out) const;

    // Any thread (the platform thread): queues one action for the UI thread to execute. Bounded and
    // overflow-dropping: a flooding client cannot grow the queue. `text` is the `SetValue` payload;
    // passing one for another action is refused, as is a payload at or over
    // `kMaxProviderActionTextBytes` - a truncated field text would leave the screen reader holding
    // a buffer the engine does not.
    [[nodiscard]] bool post_action(int virtual_id, Action action, std::string_view text = {});
    // UI thread, tick boundary: drains the queue in arrival order.
    [[nodiscard]] std::size_t take_actions(std::span<ProviderAction> out);

    // Number of snapshots published; strictly increasing. The platform watches it to know when to
    // raise one content-changed event instead of polling the tree.
    [[nodiscard]] std::uint64_t publishes() const;
    [[nodiscard]] std::uint64_t dropped_actions() const;

    // Drops the served tree and every queued action. Safe twice.
    void clear();

private:
    // The index into `nodes_` serving `virtual_id`, or kMaxNodes for an unknown/stale id.
    [[nodiscard]] std::size_t index_of(int virtual_id) const;
    // Rebuilds the slot table for the newly published tree. Called with `mutex_` held.
    void assign_slots();

    mutable std::mutex mutex_;
    // The served tree, in snapshot pre-order.
    A11ySnapshot nodes_;
    // `slot_of_node_` maps a scene node id to the virtual id it keeps across publishes.
    // `index_of_slot_[slot]` is the pre-order index serving that slot; kMaxNodes means free.
    std::array<ui::NodeId, kMaxNodes + 1> slot_of_node_{};
    std::array<std::uint16_t, kMaxNodes + 1> index_of_slot_{};
    // `virtual_id_of_index_[i]` is the slot serving pre-order index i.
    std::array<int, kMaxNodes> virtual_id_of_index_{};
    std::size_t node_count_ = 0;

    std::array<ProviderAction, kMaxProviderActions> actions_{};
    std::size_t action_count_ = 0;
    std::uint64_t publishes_ = 0;
    std::uint64_t dropped_actions_ = 0;
    bool has_snapshot_ = false;
};

} // namespace tessera::ui::a11y
