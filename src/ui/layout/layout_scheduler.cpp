// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_scheduler.hpp"

namespace tessera::ui::layout {

LayoutScheduler::LayoutScheduler(LayoutEngine& engine)
    : engine_(&engine) {}

const LayoutResult& LayoutScheduler::update(const LayoutInput& root, const LayoutViewport& viewport,
                                            bool needs_layout) {
    const bool viewport_changed = viewport.width != viewport_.width ||
                                  viewport.height != viewport_.height ||
                                  viewport.dpi_scale != viewport_.dpi_scale;
    if (valid_ && !needs_layout && !viewport_changed) {
        ++cache_hits_;
        return last_;
    }

    last_ = engine_->layout(root, viewport);
    viewport_ = viewport;
    valid_ = true;
    ++layout_runs_;
    return last_;
}

void LayoutScheduler::invalidate() {
    valid_ = false;
    last_ = LayoutResult{};
}

} // namespace tessera::ui::layout
