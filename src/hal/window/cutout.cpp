// SPDX-License-Identifier: MIT

#include "tessera/hal/cutout.hpp"

#include "tessera/hal/caption.hpp"

#include <cmath>
#include <cstdio>

namespace tessera::hal {

namespace {

bool usable(const WindowRect& rect) noexcept {
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
           std::isfinite(rect.height) && rect.width > 0.0f && rect.height > 0.0f;
}

void append(std::string& out, const char* format, double a, double b = 0, double c = 0,
            double d = 0) {
    char buffer[160];
    const int written = std::snprintf(buffer, sizeof buffer, format, a, b, c, d);
    if (written > 0) {
        out.append(buffer, static_cast<std::size_t>(written) < sizeof buffer
                               ? static_cast<std::size_t>(written)
                               : sizeof buffer - 1);
    }
}

} // namespace

CutoutInfo make_cutout_info(std::span<const WindowRect> rects,
                            const WindowInsets& waterfall_insets) noexcept {
    CutoutInfo info;
    info.waterfall_insets = waterfall_insets;
    for (const WindowRect& rect : rects) {
        if (info.count == kMaxCutouts) {
            break;
        }
        if (usable(rect)) {
            info.bounds[info.count++] = rect;
        }
    }
    return info;
}

std::string format_window_metrics(const WindowMetrics& metrics) {
    std::string out;
    append(out, "logical %gx%g\n", metrics.logical_width, metrics.logical_height);
    append(out, "scale %g\n", metrics.dpi_scale);
    append(out, "insets %g %g %g %g\n", metrics.insets.left, metrics.insets.top,
           metrics.insets.right, metrics.insets.bottom);
    const CutoutInfo& cutouts = metrics.cutouts;
    out += "cutouts " + std::to_string(cutouts.count) + "\n";
    for (std::size_t i = 0; i < cutouts.count && i < kMaxCutouts; ++i) {
        const WindowRect& rect = cutouts.bounds[i];
        append(out, "cutout %g %g %g %g\n", rect.x, rect.y, rect.width, rect.height);
    }
    append(out, "waterfall %g %g %g %g\n", cutouts.waterfall_insets.left,
           cutouts.waterfall_insets.top, cutouts.waterfall_insets.right,
           cutouts.waterfall_insets.bottom);
    append(out, "ime %g %g %g %g\n", metrics.ime_insets.left, metrics.ime_insets.top,           metrics.ime_insets.right, metrics.ime_insets.bottom);
    append(out, "gestures %g %g %g %g\n", metrics.gesture_insets.left, metrics.gesture_insets.top,
           metrics.gesture_insets.right, metrics.gesture_insets.bottom);
    const FoldInfo& fold = metrics.fold;
    append(out, "fold %g %g %g %g\n", static_cast<double>(fold.state),
           static_cast<double>(fold.orientation), static_cast<double>(fold.occlusion),
           fold.separating ? 1.0 : 0.0);
    append(out, "hinge %g %g %g %g\n", fold.hinge_bounds.x, fold.hinge_bounds.y,
           fold.hinge_bounds.width, fold.hinge_bounds.height);
    // Window mode and the custom caption's regions (issue #232): the harness needs both without a
    // display, and one line per region keeps the dump diffable.
    out += "fullscreen ";
    out += metrics.fullscreen ? "1\n" : "0\n";
    out += "caption_regions " + std::to_string(metrics.caption_region_count) + "\n";
    for (std::size_t i = 0; i < metrics.caption_region_count && i < kMaxCaptionRegions; ++i) {
        const CaptionRegion& region = metrics.caption_regions[i];
        out += "caption ";
        out += caption_region_kind_name(region.kind);
        append(out, " %g %g %g %g\n", region.bounds.x, region.bounds.y, region.bounds.width,
               region.bounds.height);
    }
    return out;
}

} // namespace tessera::hal
