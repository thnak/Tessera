// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"

namespace tessera::app {

// One restyle's view of the host's style provider (issue #711). Layout input, scene sync,
// interaction sync and the transition/animation bridge all read the same computed style of the
// same node in one frame. Between `begin` and `end` the scene style tree memoises each node's
// cascade style from its parent's, so a node costs one compute (not one per ancestor) and every
// reader gets a reference instead of a copy. The memo lives only for one restyle, so a style input
// edited between frames can never be served stale.
class MemoStyleProvider final : public ui::paint::StyleProvider {
public:
    void begin(ui::paint::SceneStyleProvider& inner) {
        inner_ = &inner;
        inner.begin_restyle();
    }
    void end() {
        if (inner_ != nullptr) {
            inner_->end_restyle();
            inner_ = nullptr;
        }
    }

    [[nodiscard]] ui::style::ComputedStyle compute(const ui::Node& node) override {
        return inner_->compute(node);
    }

    // Valid until `end`.
    [[nodiscard]] const ui::style::ComputedStyle& resolve(const ui::Node& node) override {
        return inner_->resolve(node);
    }

private:
    ui::paint::SceneStyleProvider* inner_ = nullptr;
};

} // namespace tessera::app
