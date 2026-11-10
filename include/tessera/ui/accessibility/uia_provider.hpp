// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"
#include "tessera/ui/accessibility/uia_error.hpp"
#include "tessera/ui/accessibility/uia_mapping.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>

namespace tessera::ui::a11y {

// One action a UIA client asked for. The bridge validates and queues it; the UI thread executes it
// at a tick boundary (never on the bridge thread, never re-entrant). `text` carries a
// `ValuePattern.SetValue` payload (bounded, NUL-terminated); `value` carries the numeric form for
// hosts that model range values.
struct UiaActionRequest {
    A11yIndex node = kInvalidIndex;
    Action action = Action::None;
    double value = 0.0;
    std::array<char, kMaxTextBytes> text{};
};

[[nodiscard]] std::string_view text_of(const UiaActionRequest& request);

inline constexpr std::size_t kMaxPendingUiaActions = 32;

// One snapshot node projected into the shape the Windows provider answers: control type, patterns,
// bounded name/value/description, bounds, and the state bits UIA exposes as properties. `offscreen`
// is always false in this slice because the snapshot carries world bounds, not a viewport.
struct UiaElementInfo {
    A11yIndex index = kInvalidIndex;
    A11yIndex parent = kInvalidIndex;
    UiaControlType control_type = UiaControlType::Pane;
    UiaPatterns patterns = UiaPatterns::None;
    std::array<char, kMaxTextBytes> name{};
    std::array<char, kMaxTextBytes> value{};
    std::array<char, kMaxTextBytes> description{};
    rendering::rhi::Rect bounds{};
    std::uint64_t automation_id = 0;
    std::uint16_t index_in_parent = 0;
    bool enabled = true;
    bool focused = false;
    bool selected = false;
    bool offscreen = false;
};

[[nodiscard]] std::string_view name_of(const UiaElementInfo& element);
[[nodiscard]] std::string_view value_of(const UiaElementInfo& element);
[[nodiscard]] std::string_view description_of(const UiaElementInfo& element);

// Bridge-thread view over the latest immutable semantics snapshot. It never reads the scene and
// never invokes an action: `request_action` only appends to a bounded, coalescing ring that the UI
// thread drains with `take_actions`. Every query asserts thread ownership, so a host cannot read a
// half-published tree from the UI thread.
class UiaProvider {
public:
    UiaProvider();
    UiaProvider(const UiaProvider&) = delete;
    UiaProvider& operator=(const UiaProvider&) = delete;

    // Bridge thread: adopts `snapshot` by swap. The provider's previous buffer is left in
    // `snapshot`, so a steady-state publish/take cycle allocates nothing.
    void set_snapshot(A11ySnapshot& snapshot);
    // Bridge thread identity; an empty id disables the ownership check (standalone use in tests).
    void adopt_thread(std::thread::id thread);
    [[nodiscard]] bool owns_thread() const;
    [[nodiscard]] std::expected<void, UiaError> check_thread() const;

    [[nodiscard]] bool has_snapshot() const;
    [[nodiscard]] std::size_t node_count() const;
    // First entry of the snapshot (pre-order root), or kInvalidIndex for an empty snapshot.
    [[nodiscard]] A11yIndex root() const;
    [[nodiscard]] A11yIndex parent_of(A11yIndex index) const;
    [[nodiscard]] std::uint16_t index_in_parent(A11yIndex index) const;
    [[nodiscard]] const A11yNode* node(A11yIndex index) const;
    [[nodiscard]] const A11ySnapshot& snapshot() const;
    // Increments once per adopted snapshot; a client can key cached properties on it.
    [[nodiscard]] std::uint64_t generation() const;

    // Bridge thread: the UIA projection of one node, or `NoSnapshot`/`UnknownNode`.
    [[nodiscard]] std::expected<UiaElementInfo, UiaError> element(A11yIndex index) const;
    [[nodiscard]] std::expected<std::uint16_t, UiaError> child_count(A11yIndex index) const;
    [[nodiscard]] std::expected<A11yIndex, UiaError> child_at(A11yIndex index,
                                                              std::uint16_t child) const;
    // First node with `State::Focused`, else the snapshot root.
    [[nodiscard]] std::expected<A11yIndex, UiaError> focused() const;
    // Topmost node whose bounds contain the point (reverse pre-order), else the root.
    [[nodiscard]] std::expected<A11yIndex, UiaError> hit_test(double x, double y) const;

    // Bridge thread: validate `action` against the node and queue it for the UI thread. Identical
    // (node, action) requests coalesce, so a spamming client cannot grow the ring.
    [[nodiscard]] std::expected<void, UiaError>
    request_action(A11yIndex index, Action action, double value = 0.0, std::string_view text = {});
    // UI thread: drains up to `out.size()` requests, oldest first.
    [[nodiscard]] std::size_t take_actions(std::span<UiaActionRequest> out);
    [[nodiscard]] std::size_t queued_actions() const;
    [[nodiscard]] std::uint64_t coalesced_actions() const;

private:
    void rebuild_parents();
    bool queue_locked(const UiaActionRequest& request);

    A11ySnapshot snapshot_;
    // Index -> parent index for the whole snapshot (`kInvalidIndex` for the root).
    std::array<A11yIndex, kMaxNodes> parents_{};
    std::uint64_t generation_ = 0;
    std::thread::id thread_{};

    mutable std::mutex actions_mutex_;
    std::array<UiaActionRequest, kMaxPendingUiaActions> actions_{};
    std::size_t action_head_ = 0;
    std::size_t action_count_ = 0;
    std::uint64_t coalesced_ = 0;
};

} // namespace tessera::ui::a11y
