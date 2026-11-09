// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/path_scale.hpp"

namespace tessera::rendering::path {

std::vector<PathShape> scale_path_shapes(std::span<const PathShape> shapes, float scale) {
    if (!(scale > 0.0f) || scale == 1.0f) {
        return {};
    }
    std::vector<PathShape> scaled(shapes.begin(), shapes.end());
    for (PathShape& shape : scaled) {
        for (PathSegment& segment : shape.path.segments) {
            segment.p0 = {segment.p0.x * scale, segment.p0.y * scale};
            segment.p1 = {segment.p1.x * scale, segment.p1.y * scale};
            segment.p2 = {segment.p2.x * scale, segment.p2.y * scale};
        }
        if (shape.paint.stroke.has_value()) {
            shape.paint.stroke->width *= scale;
        }
    }
    return scaled;
}

} // namespace tessera::rendering::path
