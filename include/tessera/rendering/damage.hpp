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
