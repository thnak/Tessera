// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

// `scroll_lock` (ADR-064 item 7, issue #633): stop the background from scrolling while a modal
// layer is open.
//
// The engine owns the mechanism: a node with `scrollable_y`/`scrollable_x` clear is not a scroll
// container, so `scroll_target` skips it and a wheel or gesture chains past it to the next
// ancestor. This behavior is the hook's half: it clears the target's scroll axes while `active`,
// and restores them when it is released.
//
// **Why the lock is reference-counted across behaviors.** Two dialogs can be open at once (a
// confirmation over a settings sheet), and the inner one closing must not unlock the page the outer
// one still needs locked. The count lives in one process-wide table keyed by the node, because the
// alternative — a count per behavior — cannot see the other behavior. The table is bounded and
// allocation-free on the lock path: it is a fixed array of `{node, count, axes}` records, and a
// behavior that cannot get a slot leaves the node unlocked rather than lying about it.
namespace tessera::ui::behavior {

class ScrollLock final : public Behavior {
public:
    // The manifest's output order: `locked`.
    static constexpr std::size_t kOutputCount = 1;

    ScrollLock() = default;
    ~ScrollLock() override;

    ScrollLock(const ScrollLock&) = delete;
    ScrollLock& operator=(const ScrollLock&) = delete;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

    // The node this behavior is attached to, or nullptr when detached.
    [[nodiscard]] const Node* target() const {
        return target_;
    }

    // How many behaviors currently hold a lock on `node`. Exposed so a test can assert the
    // reference-count rule without reaching into the table.
    [[nodiscard]] static std::size_t lock_count(const Node& node);

private:
    // Applies `active` to the shared count and recomputes the output.
    void apply(bool active);

    Node* target_ = nullptr;
    // Whether this behavior currently holds one reference on the target.
    bool held_ = false;
    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
