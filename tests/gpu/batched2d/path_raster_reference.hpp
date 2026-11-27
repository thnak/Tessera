// SPDX-License-Identifier: MIT
//
// The ThorVG-side twin of a GPU path scene (issue #685): the same geometry as PathShape values for
// the PathRasterizer seam, with the dashes cut on the CPU. ThorVG's seam has no dash (path_model.hpp),
// so a dashed GPU shape becomes the same dashes as open sub-paths stroked with the shape's own cap
// and join, as in tools/poc/compute_raster. The pattern restarts at the start of each contour and
// does not wrap into a join at a closed contour's end (SVG).

#pragma once

#include "tessera/rendering/path/gpu_path_scene.hpp"
#include "tessera/rendering/path/path_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::path_corpus {

namespace path = tessera::rendering::path;

struct Contour {
    std::size_t first = 0; // index of the first segment
    std::size_t count = 0;
    bool open = false;
};

[[nodiscard]] inline std::vector<Contour> contours_of(const path::GpuPathShape& shape) {
    const std::size_t total = shape.contours.size() / 4;
    std::vector<Contour> result;
    if (shape.contour_sizes.empty()) {
        result.push_back({0, total, shape.open});
        return result;
    }
    std::size_t at = 0;
    for (std::size_t k = 0; k < shape.contour_sizes.size(); ++k) {
        result.push_back({at, shape.contour_sizes[k], shape.contour_open[k]});
        at += shape.contour_sizes[k];
    }
    return result;
}

[[nodiscard]] inline path::Rgba8 unpack_rgba(std::uint32_t word) {
    return {static_cast<std::uint8_t>(word & 0xFFU), static_cast<std::uint8_t>((word >> 8) & 0xFFU),
            static_cast<std::uint8_t>((word >> 16) & 0xFFU),
            static_cast<std::uint8_t>((word >> 24) & 0xFFU)};
}

inline void append_polyline(path::Path& out, const std::vector<path::PointF>& points, bool close) {
    if (points.size() < 2) {
        return;
    }
    out.segments.push_back({path::SegmentKind::move_to, points[0], {}, {}});
    for (std::size_t i = 1; i < points.size(); ++i) {
        out.segments.push_back({path::SegmentKind::line_to, points[i], {}, {}});
    }
    if (close) {
        out.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    }
}

// The dashes of one contour (segments first..first+count) as polylines.
[[nodiscard]] inline std::vector<std::vector<path::PointF>>
dash_runs(const path::GpuPathShape& shape, const Contour& contour) {
    std::vector<std::vector<path::PointF>> runs;
    const std::vector<float>& pattern = shape.dash;
    double period = 0.0;
    for (const float v : pattern) {
        period += static_cast<double>(v);
    }
    if (period <= 0.0) {
        return runs;
    }
    std::size_t k = 0;
    double phase = std::fmod(static_cast<double>(shape.dash_offset), period);
    if (phase < 0.0) {
        phase += period;
    }
    double c0 = 0.0;
    while (k < pattern.size() && phase >= c0 + static_cast<double>(pattern[k])) {
        c0 += static_cast<double>(pattern[k++]);
    }
    if (k >= pattern.size()) {
        k = 0;
        c0 = 0.0;
        phase = 0.0;
    }
    double remaining = c0 + static_cast<double>(pattern[k]) - phase;
    std::vector<path::PointF> run;
    const auto flush = [&] {
        if (run.size() >= 2) {
            runs.push_back(run);
        }
        run.clear();
    };
    for (std::size_t s = contour.first; s < contour.first + contour.count; ++s) {
        const float* seg = &shape.contours[s * 4];
        const double x0 = static_cast<double>(seg[0]);
        const double y0 = static_cast<double>(seg[1]);
        const double dx = static_cast<double>(seg[2]) - x0;
        const double dy = static_cast<double>(seg[3]) - y0;
        const double length = std::sqrt(dx * dx + dy * dy);
        double done = 0.0;
        while (done < length - 1e-9) {
            const double step = std::min(remaining, length - done);
            const bool on = k % 2 == 0;
            if (on && step > 0.0) {
                if (run.empty()) {
                    run.push_back({static_cast<float>(x0 + dx * done / length),
                                   static_cast<float>(y0 + dy * done / length)});
                }
                run.push_back({static_cast<float>(x0 + dx * (done + step) / length),
                               static_cast<float>(y0 + dy * (done + step) / length)});
            }
            done += step;
            remaining -= static_cast<double>(step);
            if (remaining <= 1e-9) {
                if (on) {
                    flush();
                }
                k = (k + 1) % pattern.size();
                remaining = static_cast<double>(pattern[k]);
            }
        }
    }
    flush();
    return runs;
}

[[nodiscard]] inline path::LineCap to_cap(path::GpuCap cap) {
    switch (cap) {
    case path::GpuCap::round:
        return path::LineCap::round;
    case path::GpuCap::square:
        return path::LineCap::square;
    case path::GpuCap::butt:
        break;
    }
    return path::LineCap::butt;
}

[[nodiscard]] inline path::LineJoin to_join(path::GpuJoin join) {
    switch (join) {
    case path::GpuJoin::miter:
        return path::LineJoin::miter;
    case path::GpuJoin::bevel:
        return path::LineJoin::bevel;
    case path::GpuJoin::round:
        break;
    }
    return path::LineJoin::round;
}

[[nodiscard]] inline path::PathShape reference_shape(const path::GpuPathShape& shape) {
    path::PathShape result;
    const path::Rgba8 color = unpack_rgba(shape.rgba);
    const bool stroked = shape.half_width > 0.0f;
    result.path.fill_rule = shape.rule == path::GpuFillRule::even_odd ? path::FillRule::even_odd
                                                                      : path::FillRule::non_zero;
    for (const Contour& contour : contours_of(shape)) {
        if (contour.count == 0) {
            continue;
        }
        if (stroked && !shape.dash.empty()) {
            for (const auto& run : dash_runs(shape, contour)) {
                append_polyline(result.path, run, false);
            }
            continue;
        }
        std::vector<path::PointF> points;
        points.push_back({shape.contours[contour.first * 4], shape.contours[contour.first * 4 + 1]});
        for (std::size_t s = contour.first; s < contour.first + contour.count; ++s) {
            points.push_back({shape.contours[s * 4 + 2], shape.contours[s * 4 + 3]});
        }
        if (!contour.open && points.size() > 2) {
            points.pop_back(); // the closing edge returns to the start; close() draws it
        }
        append_polyline(result.path, points, !contour.open);
    }
    if (stroked) {
        path::StrokeStyle style;
        style.color = color;
        style.width = shape.half_width * 2.0f;
        style.cap = to_cap(shape.cap);
        style.join = to_join(shape.join);
        style.miter_limit = shape.miter_limit;
        result.paint.stroke = style;
    } else {
        result.paint.fill = color;
    }
    return result;
}

[[nodiscard]] inline std::vector<path::PathShape> reference_shapes(const path::GpuPathScene& scene) {
    std::vector<path::PathShape> shapes;
    for (const path::GpuPathShape& shape : scene.shapes) {
        shapes.push_back(reference_shape(shape));
    }
    return shapes;
}

} // namespace tessera::path_corpus
