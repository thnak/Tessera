// SPDX-License-Identifier: MIT

#pragma once

// Inline half of `NodeGeometryApi`. Included only by `node.hpp`, after `Node` is complete. Do not
// include this file directly.

namespace tessera::ui {

inline Node& NodeGeometryApi::node() noexcept {
    return static_cast<Node&>(*this);
}
inline const Node& NodeGeometryApi::node() const noexcept {
    return static_cast<const Node&>(*this);
}

inline const rhi::Rect& NodeGeometryApi::bounds() const {
    return node().bounds_;
}
inline const rhi::Mat3& NodeGeometryApi::transform() const {
    return node().placement_.transform();
}
inline bool NodeGeometryApi::has_transform() const {
    return node().hints_.has_transform();
}
inline int NodeGeometryApi::z_index() const {
    return node().placement_.z_index();
}
inline bool NodeGeometryApi::has_z_index() const {
    return node().hints_.has_z_index();
}
inline const PaintOutsets& NodeGeometryApi::paint_outsets() const {
    return node().extent_.outsets();
}
inline float NodeGeometryApi::glass_blur_sigma() const {
    return node().extent_.glass_blur_sigma();
}
inline bool NodeGeometryApi::glass_subtree() const {
    return node().hints_.glass_subtree();
}
inline float NodeGeometryApi::filter_reach() const {
    return node().extent_.filter_reach();
}
inline bool NodeGeometryApi::filter_subtree() const {
    return node().hints_.filter_subtree();
}
inline bool NodeGeometryApi::text_fits_box() const {
    return node().hints_.text_fits_box();
}

} // namespace tessera::ui
