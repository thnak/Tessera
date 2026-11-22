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
    return node().transform_;
}
inline int NodeGeometryApi::z_index() const {
    return node().z_index_;
}
inline const PaintOutsets& NodeGeometryApi::paint_outsets() const {
    return node().paint_outsets_;
}
inline float NodeGeometryApi::glass_blur_sigma() const {
    return node().glass_blur_sigma_;
}
inline bool NodeGeometryApi::glass_subtree() const {
    return node().flag(Node::kGlassSubtree);
}
inline bool NodeGeometryApi::text_fits_box() const {
    return node().flag(Node::kTextFitsBox);
}

} // namespace tessera::ui
