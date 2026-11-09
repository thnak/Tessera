// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_model.hpp"

namespace tessera::rendering::path {

// Shared segment-stream validation: a sub-path starts with move_to and ends with close or with
// the end of the list; every drawing command needs an open sub-path. Backends reject inputs with
// the same rule, so the seam behaves identically with and without a third-party rasterizer.
[[nodiscard]] inline bool validate_path(const Path& path) {
    bool in_subpath = false;
    for (const PathSegment& segment : path.segments) {
        switch (segment.kind) {
        case SegmentKind::move_to:
            in_subpath = true;
            break;
        case SegmentKind::line_to:
        case SegmentKind::quad_to:
        case SegmentKind::cubic_to:
            if (!in_subpath) {
                return false;
            }
            break;
        case SegmentKind::close:
            if (!in_subpath) {
                return false;
            }
            in_subpath = false;
            break;
        }
    }
    return true;
}

// Paint validation shared by the backends: a stroke needs a positive width and a non-negative
// miter limit. Fill and stroke are optional; an unpainted shape is legal (it draws nothing).
[[nodiscard]] inline bool validate_paint(const PaintStyle& paint) {
    if (!paint.stroke.has_value()) {
        return true;
    }
    return paint.stroke->width > 0.0f && paint.stroke->miter_limit >= 0.0f;
}

} // namespace tessera::rendering::path
