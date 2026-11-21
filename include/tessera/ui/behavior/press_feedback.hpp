// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

// `press_feedback` (spike #658): the thinnest complete behavior, used to prove the path end to end.
//
// It reports whether the target is currently pressed, hovered or focused, which is what a Button
// needs to pick its `:active`/`:hover`/`:focus-visible` visual without a stylesheet pseudo-class
// reaching into the scene. The three outputs are independent: a pointer press on a hovered button
// reports both, and a keyboard activation reports `pressed` without `hovered`.
//
// The behavior owns no listeners of its own: the scene's input dispatch already normalizes pointer,
// touch and keyboard input, so `press_feedback` is a pure function of the node's interaction state.
// That is what makes it the right first behavior: it proves the attach/update/detach seam without
// also proving a new event subscription mechanism.
namespace tessera::ui::behavior {

class PressFeedback final : public Behavior {
public:
    // The manifest's output order: `pressed`, `hovered`, `focused`.
    static constexpr std::size_t kOutputCount = 3;

    PressFeedback() = default;

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
    // Recomputes the three outputs from the node's interaction state. Called from `attach` and from
    // `refresh`, which the scene calls after a dispatch; it allocates nothing.
    void refresh();

    Node* target_ = nullptr;
    // The scene's interaction state, handed in at attach; null when the build has no scene.
    InputRouter* router_ = nullptr;
    // The `disabled` option: when true the target never reports `pressed`, but still reports
    // `hovered` and `focused` (ADR-064 item 9: a disabled control stays focusable and explains
    // why).
    bool disabled_ = false;
    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
