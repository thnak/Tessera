// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

// `interactive_role` (ADR-064 item 7, issue #635): tell a child it sits inside an interactive
// parent.
//
// The problem it solves is a real accessibility defect, not a convenience: a card whose whole
// surface is clickable, containing a button, produces a button inside a button. A screen reader
// announces two nested controls, and a pointer press on the inner button also triggers the outer
// card's handler because the click bubbles. The hook marks the outer element as the interactive one
// and reports to its descendants that they are nested, so a component can render the inner control
// as a plain element (or move it out of the clickable region) instead of nesting a second control.
//
// **Why the state is on the node, not in the behavior.** A descendant needs the answer, and a
// descendant is not the component that called the hook: it is a different component, possibly in a
// different module. The only thing they share is the scene, so the flag lives on the node
// (`Node::set_interactive_parent`) and a descendant reads it by walking its ancestors. A behavior
// that kept the flag privately could not answer the question the hook exists to answer.
namespace tessera::ui::behavior {

class InteractiveRole final : public Behavior {
public:
    // The manifest's output order: `nested`.
    static constexpr std::size_t kOutputCount = 1;

    InteractiveRole() = default;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

    // The node this behavior is attached to, or nullptr when detached.
    [[nodiscard]] const Node* target() const {
        return target_;
    }

    // Whether `node` has an interactive ancestor. A component calls this to decide whether to
    // render its own control or a plain element. Public and static because the caller is a
    // descendant, not the behavior's owner.
    [[nodiscard]] static bool has_interactive_ancestor(const Node& node);

private:
    void apply(bool interactive);

    Node* target_ = nullptr;
    bool applied_ = false;
    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
