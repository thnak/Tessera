// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

// `roving_focus` (ADR-064 item 7, issue #625): one Tab stop for a group of items, with the arrow
// keys moving focus among them.
//
// The engine owns the mechanism: `Node::set_focus_group` marks a node as a roving-focus group and
// `FocusManager::move_in_focus_group` moves focus along the group's axis, wrapping at both ends and
// honouring Home/End. The manager also remembers which item holds the group's Tab stop, so
// re-entering the group returns to the item the user left.
//
// This behavior is the hook's half: it sets the group's axis from the `orientation` option and
// reports the index of the item that currently holds the Tab stop, which is what a component needs
// to render `aria-activedescendant` or a highlight.
//
// **Why the index is computed here and not read from the manager.** The manager stores the Tab stop
// as a `NodeId` in a private map keyed by group; it has no public "which index of this group"
// query, and adding one would put a component concern in the scene layer. The behavior already
// knows the group's subtree, so it walks it and reports the position of the focused item.
namespace tessera::ui::behavior {

class RovingFocus final : public Behavior {
public:
    // The manifest's output order: `activeIndex`.
    static constexpr std::size_t kOutputCount = 1;

    RovingFocus() = default;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

    // The node this behavior is attached to, or nullptr when detached.
    [[nodiscard]] const Node* target() const {
        return target_;
    }

private:
    // Applies the `orientation` option to the target's group axis and recomputes the output.
    void apply(std::string_view orientation);
    // The index of the item that holds the group's Tab stop, or -1 when none does.
    [[nodiscard]] int active_index() const;

    Node* target_ = nullptr;
    InputRouter* router_ = nullptr;
    // The axis this behavior last applied, so `detach` restores the node to a non-group rather than
    // leaving a group behind for a component that no longer exists.
    bool applied_ = false;
    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
