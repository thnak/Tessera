// SPDX-License-Identifier: MIT

#include "tessera/rendering/damage.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tessera::rendering {
namespace {

bool touches(const rhi::Rect& lhs, const rhi::Rect& rhs) {
    return lhs.x <= rhs.right() && rhs.x <= lhs.right() && lhs.y <= rhs.bottom() &&
           rhs.y <= lhs.bottom();
}

rhi::Rect merge(const rhi::Rect& lhs, const rhi::Rect& rhs) {
    const float x0 = std::min(lhs.x, rhs.x);
    const float y0 = std::min(lhs.y, rhs.y);
    const float x1 = std::max(lhs.right(), rhs.right());
    const float y1 = std::max(lhs.bottom(), rhs.bottom());
    return rhi::Rect{x0, y0, x1 - x0, y1 - y0};
}

} // namespace

DamageAccumulator::DamageAccumulator(DamageConfig config)
    : target_(config.target),
      max_rects_(config.max_rects) {
    if (max_rects_ == 0) {
        throw std::invalid_argument("damage max_rects must be greater than zero");
    }
    rects_.reserve(max_rects_);
}

void DamageAccumulator::add(rhi::Rect rect, float margin) {
    if (!std::isfinite(margin) || margin < 0.0f) {
        throw std::invalid_argument("damage margin must be finite and non-negative");
    }
    if (full_redraw_) {
        return;
    }

    rect.x -= margin;
    rect.y -= margin;
    rect.width += margin * 2.0f;
    rect.height += margin * 2.0f;

    const float x0 = std::max(0.0f, std::min(rect.x, rect.right()));
    const float y0 = std::max(0.0f, std::min(rect.y, rect.bottom()));
    const float x1 = std::min(static_cast<float>(target_.width), std::max(rect.x, rect.right()));
    const float y1 = std::min(static_cast<float>(target_.height), std::max(rect.y, rect.bottom()));
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    rhi::Rect clipped{x0, y0, x1 - x0, y1 - y0};
    bool merged = true;
    while (merged) {
        merged = false;
        for (auto it = rects_.begin(); it != rects_.end(); ++it) {
            if (touches(clipped, *it)) {
                clipped = merge(clipped, *it);
                rects_.erase(it);
                merged = true;
                break;
            }
        }
    }

    if (rects_.size() >= max_rects_) {
        full_redraw_ = true;
        rects_.clear();
        return;
    }
    rects_.push_back(clipped);
}

void DamageAccumulator::clear() {
    rects_.clear();
    full_redraw_ = false;
}

} // namespace tessera::rendering
