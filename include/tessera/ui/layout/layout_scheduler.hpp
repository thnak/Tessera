// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"

#include <cstddef>

namespace tessera::ui::layout {

// Incremental relayout policy: reuse the last layout result while the caller reports no layout
// dirt and the viewport is unchanged. Yoga has no partial-layout API, so any real layout dirt
// still recomputes the tree; the win is skipping paint-only/animation frames entirely.
class LayoutScheduler {
public:
    explicit LayoutScheduler(LayoutEngine& engine);

    [[nodiscard]] const LayoutResult& update(const LayoutInput& root,
                                             const LayoutViewport& viewport, bool needs_layout);
    [[nodiscard]] const LayoutResult& current() const {
        return last_;
    }
    [[nodiscard]] bool valid() const {
        return valid_;
    }
    [[nodiscard]] std::size_t layout_runs() const {
        return layout_runs_;
    }
    // Relayouts an independent subtree (overlay/popup) and patches its boxes into the cached
    // result, keeping sibling boxes untouched. Returns false when the cache is invalid or the
    // subtree root has no previous box. Structural removals inside the subtree still require
    // invalidate()/a full update; style/size changes are the intended case.
    bool relayout_subtree(const LayoutInput& root, std::uint64_t id,
                          const LayoutViewport& viewport);

    [[nodiscard]] std::size_t partial_runs() const {
        return partial_runs_;
    }
    [[nodiscard]] std::size_t cache_hits() const {
        return cache_hits_;
    }

    void invalidate();

private:
    LayoutEngine* engine_ = nullptr;
    LayoutResult last_;
    LayoutViewport viewport_;
    bool valid_ = false;
    std::size_t layout_runs_ = 0;
    std::size_t cache_hits_ = 0;
    std::size_t partial_runs_ = 0;
};

} // namespace tessera::ui::layout
