// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

namespace tessera::ui::layout {
class LayoutReadModel;
}

// `overflow` (ADR-064 item 7, issue #634): measure which children of a row overflow, and report the
// cut index.
//
// The engine owns the measurement: `layout::LayoutReadModel` holds every node's border box from the
// last layout pass, so the behavior reads sizes rather than polling or forcing a layout. This
// behavior is the hook's half: it walks the target's children in order, accumulates their widths
// against the target's own content width, and reports how many fit.
//
// **Why this is not a layout loop.** The behavior never asks for a layout: it reads the boxes the
// last pass produced. A component that renders `count` children and hides the rest therefore
// settles in one pass, because the widths it measures are the widths the previous pass already
// committed. A component that changes its children in response to `count` re-renders once, and the
// next pass measures the new set; the behavior reports the same answer if the widths did not
// change, so it does not oscillate.
//
// **Why a reserved trailing slot.** An overflow list usually ends with a "more" button, which takes
// width the items cannot use. `reserved` is that width, subtracted before the fit is computed, so
// the count is the number of items that fit *alongside* the button rather than the number that
// would fit without it.
namespace tessera::ui::behavior {

class Overflow final : public Behavior {
public:
    // The manifest's output order: `count`, `overflowing`.
    static constexpr std::size_t kOutputCount = 2;

    Overflow() = default;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

    // Recomputes the count from the read model. Called from `attach`, `update` and `refresh`;
    // public so a test can publish a layout and assert the count without a dispatch.
    void measure();

    // The node this behavior is attached to, or nullptr when detached.
    [[nodiscard]] const Node* target() const {
        return target_;
    }

private:
    Node* target_ = nullptr;
    layout::LayoutReadModel* model_ = nullptr;
    // The width the trailing slot needs, in logical pixels.
    double reserved_ = 0.0;
    // The gap between items, in logical pixels.
    double gap_ = 0.0;
    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
