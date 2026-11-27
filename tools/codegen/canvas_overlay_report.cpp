// SPDX-License-Identifier: MIT

#include "canvas_overlay_report.hpp"
#include "json_text.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string_view>

namespace tessera::compiler::codegen::cli {
namespace {

namespace hal = tessera::hal;

// One float as JSON, with no field name. `std::to_chars` never consults the locale, so a hinge edge
// prints `183.5` on a machine whose locale spells it `183,5` - which `JSON.parse` would read as two
// values and the page would draw as a band in the wrong place. The shortest round-trip form also
// prints whole numbers without a decimal point, which keeps an ordinary box edge readable.
void write_float(std::string& out, float value) {
    char buffer[40];
    const std::to_chars_result result = std::to_chars(
        buffer, buffer + sizeof buffer, static_cast<double>(value), std::chars_format::general);
    if (result.ec != std::errc{}) {
        // A non-finite edge cannot be written as JSON. `0` is the only value that keeps the
        // document parseable, and a non-finite inset is a HAL bug the page will show as an absent
        // box.
        out.push_back('0');
        return;
    }
    out.append(buffer, result.ptr);
}

void append_float(std::string& out, std::string_view name, float value) {
    out += ",\"";
    out += name;
    out += "\":";
    write_float(out, value);
}

void append_rect(std::string& out, std::string_view name, const hal::WindowRect& rect) {
    out += ",\"";
    out += name;
    out += "\":{\"x\":";
    write_float(out, rect.x);
    out += ",\"y\":";
    write_float(out, rect.y);
    out += ",\"width\":";
    write_float(out, rect.width);
    out += ",\"height\":";
    write_float(out, rect.height);
    out.push_back('}');
}

void append_insets(std::string& out, std::string_view name, const hal::WindowInsets& insets) {
    out += ",\"";
    out += name;
    out += "\":{\"left\":";
    write_float(out, insets.left);
    out += ",\"top\":";
    write_float(out, insets.top);
    out += ",\"right\":";
    write_float(out, insets.right);
    out += ",\"bottom\":";
    write_float(out, insets.bottom);
    out.push_back('}');
}

std::string_view state_name(hal::FoldState state) noexcept {
    switch (state) {
    case hal::FoldState::Flat:
        return "flat";
    case hal::FoldState::HalfOpened:
        return "half-opened";
    case hal::FoldState::None:
        break;
    }
    return "none";
}

std::string_view orientation_name(hal::FoldOrientation orientation) noexcept {
    switch (orientation) {
    case hal::FoldOrientation::Horizontal:
        return "horizontal";
    case hal::FoldOrientation::Vertical:
        return "vertical";
    case hal::FoldOrientation::None:
        break;
    }
    return "none";
}

} // namespace

std::string canvas_overlay_report_json(const hal::WindowMetrics& metrics) {
    std::string out = "{\"overlayVersion\":1";
    out += ",\"width\":";
    out += std::to_string(metrics.logical_width);
    out += ",\"height\":";
    out += std::to_string(metrics.logical_height);
    out += ",\"foldState\":";
    out += quoted(state_name(metrics.fold.state));
    out += ",\"foldOrientation\":";
    out += quoted(orientation_name(metrics.fold.orientation));
    out += ",\"foldOcclusion\":";
    out += quoted(metrics.fold.occlusion == hal::FoldOcclusion::Full ? "full" : "none");
    out += ",\"separating\":";
    out += metrics.fold.separating ? "true" : "false";
    // The hinge angle is the one continuous fold value (`os-events-v1.md` section 2). -1 is the
    // platform's "nothing measured one", which is what a simulated posture reports, and the page
    // then draws no angle readout rather than the nonsensical `-1°`.
    append_float(out, "hingeAngle", metrics.fold.hinge_angle_degrees);
    append_rect(out, "hinge", metrics.fold.hinge_bounds);
    append_insets(out, "insets", metrics.insets);
    append_insets(out, "ime", metrics.ime_insets);
    append_insets(out, "gestures", metrics.gesture_insets);
    // The cutout rectangles, capped at the HAL's own maximum. An entry past `count` is zero and is
    // not reported: a page drawing four boxes when the device reported one would show three notches
    // that are not on any device.
    out += ",\"cutouts\":[";
    const std::size_t count =
        std::min<std::size_t>(metrics.cutouts.count, static_cast<std::size_t>(hal::kMaxCutouts));
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            out.push_back(',');
        }
        const hal::WindowRect& cutout = metrics.cutouts.bounds[index];
        out += "{\"x\":";
        write_float(out, cutout.x);
        out += ",\"y\":";
        write_float(out, cutout.y);
        out += ",\"width\":";
        write_float(out, cutout.width);
        out += ",\"height\":";
        write_float(out, cutout.height);
        out.push_back('}');
    }
    out += "]}\n";
    return out;
}

} // namespace tessera::compiler::codegen::cli
