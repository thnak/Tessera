// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"

#include <unordered_map>

namespace tessera::app {

// Resolves each node once per restyle (issue #711). Layout input, scene sync, interaction sync and
// the transition/animation bridge all read the same computed style of the same node in one frame;
// resolving it for each of them costs ~900 allocations per node per call. The memo is valid only
// between `begin` and `end` (one restyle), so a style input edited between frames can never be
// served stale.
class MemoStyleProvider final : public ui::paint::StyleProvider {
public:
    void begin(ui::paint::StyleProvider& inner) {
        inner_ = &inner;
        memo_.clear();
    }
    void end() {
        inner_ = nullptr;
        memo_.clear();
    }

    [[nodiscard]] ui::style::ComputedStyle compute(const ui::Node& node) override {
        return resolve(node);
    }

    // The reference stays valid until `end`/`begin` (the map is node based).
    [[nodiscard]] const ui::style::ComputedStyle& resolve(const ui::Node& node) override {
        const auto found = memo_.find(node.id());
        if (found != memo_.end()) {
            return found->second;
        }
        return memo_.emplace(node.id(), inner_->compute(node)).first->second;
    }

private:
    ui::paint::StyleProvider* inner_ = nullptr;
    std::unordered_map<ui::NodeId, ui::style::ComputedStyle> memo_;
};

} // namespace tessera::app
