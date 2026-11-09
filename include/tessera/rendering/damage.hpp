// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <vector>

namespace tessera::rendering {

struct DamageConfig {
    rhi::Extent2D target;
    std::size_t max_rects = 16;
};

class DamageAccumulator {
public:
    explicit DamageAccumulator(DamageConfig config);

    void add(rhi::Rect rect, float margin = 0.0f);
    // Resets every piece of accumulated state: the damage rectangles, their count/area inputs to
    // the redraw policy, and the latched full-redraw fallback (set when the rect cap is exceeded).
    // Configuration (target extent, max_rects) and allocated capacity persist. After `clear()` the
    // accumulator is semantically identical to a freshly constructed one with the same
    // configuration: an empty accumulation produces an empty plan, and new damage is judged by the
    // policy from scratch. This is what keeps an idle frame idle after a whole-screen change.
    void clear();

    [[nodiscard]] const std::vector<rhi::Rect>& rects() const {
        return rects_;
    }
    [[nodiscard]] bool empty() const {
        return rects_.empty() && !full_redraw_;
    }
    [[nodiscard]] bool full_redraw() const {
        return full_redraw_;
    }
    [[nodiscard]] rhi::Extent2D target() const {
        return target_;
    }

private:
    rhi::Extent2D target_;
    std::size_t max_rects_ = 0;
    std::vector<rhi::Rect> rects_;
    bool full_redraw_ = false;
};

} // namespace tessera::rendering
