// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

// How a scene node sits among its siblings beyond its box: the local transform that acts around the
// box origin (animation, rotation, scale) and the stacking index (CSS `z-index`, `auto` = 0)
// (docs/architecture/god-type-audit-v1.md section 3.1). 40 bytes, kept off the node's first cache
// line because most nodes have neither; `NodeHints` carries the two bits that say when they matter.
//
// The type stores and compares. Both setters return whether the value changed, and the owner marks
// Paint|Render: a placement change moves pixels but never the layout.
namespace tessera::ui {

class NodePlacement {
public:
    [[nodiscard]] const rendering::rhi::Mat3& transform() const noexcept {
        return transform_;
    }
    // Returns true when the matrix changed.
    bool set_transform(const rendering::rhi::Mat3& value) noexcept {
        if (same(transform_, value)) {
            return false;
        }
        transform_ = value;
        return true;
    }
    [[nodiscard]] bool transform_is_identity() const noexcept {
        return same(transform_, rendering::rhi::Mat3::identity());
    }
    // True when the matrix maps a point by a plain translation (m[2], m[5]): the affine part is the
    // identity, so a hit test can subtract instead of inverting a matrix.
    [[nodiscard]] bool is_translation_only() const noexcept {
        return transform_.m[0] == 1.0f && transform_.m[1] == 0.0f && transform_.m[3] == 0.0f &&
               transform_.m[4] == 1.0f;
    }

    // Lower values paint first and hit last; equal values keep child order. Visual only.
    [[nodiscard]] int z_index() const noexcept {
        return z_index_;
    }
    bool set_z_index(int value) noexcept {
        if (z_index_ == value) {
            return false;
        }
        z_index_ = value;
        return true;
    }

private:
    [[nodiscard]] static bool same(const rendering::rhi::Mat3& lhs,
                                   const rendering::rhi::Mat3& rhs) noexcept {
        for (int i = 0; i < 9; ++i) {
            if (lhs.m[i] != rhs.m[i]) {
                return false;
            }
        }
        return true;
    }

    rendering::rhi::Mat3 transform_;
    int z_index_ = 0;
};

static_assert(sizeof(NodePlacement) == 40, "NodePlacement is a 36-byte matrix and an int");

} // namespace tessera::ui
