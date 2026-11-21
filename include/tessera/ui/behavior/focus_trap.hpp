// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

// `focus_trap` (ADR-064 item 7, issue #624): keep keyboard focus inside a container while it is
// active, and give it back when it is released.
//
// The engine already owns the mechanism: `Node::set_focus_trap` marks a subtree as a trap and
// `FocusManager::sync_focus_trap` makes the last visible one the modal root, which confines Tab
// traversal and restores the focus held before the trap. This behavior is the hook's half of that
// contract: it turns the trap flag on and off with the `active` option, and reports whether the
// trap currently holds focus.
//
// **Why it does not call `sync_focus_trap` itself.** That method walks the whole tree to find the
// last trap, and the host already calls it once per frame and once before each key event
// (`src/app/window_lifecycle.cpp`, `src/ui/scene/input_dispatch.cpp`). A behavior that also called
// it would make the cost depend on how many traps a component happens to declare. Setting the flag
// is the behavior's job; reconciling the tree is the host's.
namespace tessera::ui::behavior {

class FocusTrap final : public Behavior {
public:
    // The manifest's output order: `active`.
    static constexpr std::size_t kOutputCount = 1;

    FocusTrap() = default;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

    // The node this behavior is attached to, or nullptr when detached. Exposed so a test can assert
    // the attach/detach lifecycle without reaching into private state.
    [[nodiscard]] const Node* target() const {
        return target_;
    }

private:
    // Applies the `active` option to the target's trap flag and recomputes the output. Called from
    // `attach` and `update`; it allocates nothing.
    void apply(bool active);

    Node* target_ = nullptr;
    // The scene's interaction state, handed in at attach; null when the build has no scene.
    InputRouter* router_ = nullptr;
    // The trap flag as this behavior last set it, so `detach` restores the node to untrapped rather
    // than leaving a trap behind for a component that no longer exists.
    bool applied_ = false;
    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
