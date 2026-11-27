// SPDX-License-Identifier: MIT
//
// The canvas viewport limits (issue #690): one shared clamp makes a toolbar's typed size, a device
// profile's DPR and a raw `viewport.set` unable to ask the canvas for a backing store beyond what a
// render target can be. Before this, `canvas_viewport_request` refused only non-positive values, so
// a large size at a high DPR asked for a surface the device cannot create - the case the issue
// calls out as "no viewport ... may lose the device".
//
// The rule is a pure function over three numbers (`tools/codegen/canvas_viewport_request.cpp`), so
// this file needs no device, no session and no font. The page's half of the same rule ships in
// `designer/web/src/views/canvas-viewport.ts` and is asserted independently by
// `scripts/ci/preview_viewport_limits.mjs` against the same numbers: the two must not drift, and
// the test below is the engine's copy of the contract.

#include "canvas_viewport_request.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "canvas_viewport_limit: " << message << "\n";
        ++failures;
    }
}

void expect_near(float actual, float wanted, std::string_view message) {
    if (std::fabs(actual - wanted) > 1e-3F) {
        std::cerr << "canvas_viewport_limit: " << message << " (got " << actual << ", wanted "
                  << wanted << ")\n";
        ++failures;
    }
}

bool has_error(const cli::CanvasViewportRequest& request) {
    return !request.error.empty();
}

// A request inside every bound is passed through untouched, and reports itself as unclamped: the
// clamp is only allowed to be visible when it changed something.
void an_ordinary_request_is_unchanged() {
    const cli::CanvasViewportRequest request = cli::canvas_viewport_request(390.0F, 844.0F, 2.0F);
    expect(!has_error(request), "a phone-sized request is usable");
    expect(!request.clamped, "a phone-sized request is not clamped");
    expect_near(request.width, 390.0F, "the width is kept");
    expect_near(request.height, 844.0F, "the height is kept");
    expect_near(request.dpi, 2.0F, "the DPR is kept");
}

// Exactly at a limit is inside it: `<=` not `<`, so a profile that names the maximum gets what it
// asked for and never a warning it did not need.
void the_limits_themselves_are_legal() {
    const cli::CanvasViewportRequest request = cli::canvas_viewport_request(
        cli::kViewportMaxExtent, cli::kViewportMaxExtent, cli::kViewportMaxDpr);
    expect(!has_error(request), "the maximum extent and DPR are usable");
    // 8192 x 8192 x 4 x 4 is 1 GiB, so the byte cap reduces the DPR even though each field is
    // legal.
    expect(request.clamped, "the byte cap clamps the largest legal rectangle");
    expect_near(request.width, cli::kViewportMaxExtent, "the width stays at the maximum");
    expect_near(request.height, cli::kViewportMaxExtent, "the height stays at the maximum");
    const double bytes = static_cast<double>(request.width) * static_cast<double>(request.height) *
                         static_cast<double>(request.dpi) * static_cast<double>(request.dpi) * 4.0;
    expect(bytes <= static_cast<double>(cli::kViewportMaxBackingBytes) * 1.001,
           "the clamped backing store fits the byte cap");
}

// Above the extent limit both axes clamp, and the request says so: a clamp is reported, never
// silent.
void a_huge_extent_clamps_instead_of_failing() {
    const cli::CanvasViewportRequest request =
        cli::canvas_viewport_request(100000.0F, 200000.0F, 1.0F);
    expect(!has_error(request), "a huge extent is clamped, not refused");
    expect(request.clamped, "the clamp is reported");
    expect_near(request.width, cli::kViewportMaxExtent, "the width clamps to the maximum");
    expect_near(request.height, cli::kViewportMaxExtent, "the height clamps to the maximum");
}

// A DPR over the limit clamps the scale, not the layout: a clamp must not move a breakpoint.
void a_high_dpr_clamps_the_scale_only() {
    const cli::CanvasViewportRequest request = cli::canvas_viewport_request(320.0F, 240.0F, 100.0F);
    expect(!has_error(request), "a high DPR is clamped, not refused");
    expect(request.clamped, "the DPR clamp is reported");
    expect_near(request.width, 320.0F, "the layout width is untouched by the DPR clamp");
    expect_near(request.height, 240.0F, "the layout height is untouched by the DPR clamp");
    expect_near(request.dpi, cli::kViewportMaxDpr, "the DPR clamps to the maximum");
}

// The memory ceiling is the one clamp a single field cannot show. A rectangle that is legal on both
// axes but too large once multiplied must still fit.
void the_byte_cap_reduces_the_dpr() {
    const cli::CanvasViewportRequest request = cli::canvas_viewport_request(4096.0F, 4096.0F, 4.0F);
    expect(!has_error(request), "a large rectangle is usable");
    expect(request.clamped, "the byte cap is reported");
    expect_near(request.width, 4096.0F, "the layout width is kept");
    expect_near(request.height, 4096.0F, "the layout height is kept");
    expect(request.dpi < 4.0F, "the DPR was reduced to fit");
    const double bytes = static_cast<double>(request.width) * static_cast<double>(request.height) *
                         static_cast<double>(request.dpi) * static_cast<double>(request.dpi) * 4.0;
    expect(bytes <= static_cast<double>(cli::kViewportMaxBackingBytes) * 1.001,
           "the reduced backing store fits the byte cap");
}

// A value that names no size is refused, never clamped: "the largest legal size" is an answer to a
// question the caller did not ask, and a zero would lay the tree out at a rectangle nobody chose.
void zero_and_negative_are_refused() {
    for (const float bad : {0.0F, -1.0F}) {
        const cli::CanvasViewportRequest request = cli::canvas_viewport_request(bad, 240.0F, 1.0F);
        expect(has_error(request), "a non-positive extent is refused");
        expect(!request.clamped, "a refusal is not a clamp");
    }
    const cli::CanvasViewportRequest zero_height = cli::canvas_viewport_request(320.0F, 0.0F, 1.0F);
    expect(has_error(zero_height), "a zero height is refused");
}

// NaN and infinity can arrive from a typed field or a bad computation; they must be refused without
// reaching `std::clamp`, whose ordering with a NaN is undefined.
void non_finite_values_are_refused() {
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    expect(has_error(cli::canvas_viewport_request(kNan, 240.0F, 1.0F)), "a NaN width is refused");
    expect(has_error(cli::canvas_viewport_request(320.0F, kInf, 1.0F)),
           "an infinite height is refused");
    expect(has_error(cli::canvas_viewport_request(320.0F, 240.0F, kNan)), "a NaN DPR is refused");
    expect(has_error(cli::canvas_viewport_request(320.0F, 240.0F, -kInf)),
           "an infinite DPR is refused");
}

// The minimum is a real, visible device: a 1x1 request is legal (the smallest the issue names), and
// the smallest DPR is inside the limit rather than below it.
void the_minimum_is_legal() {
    const cli::CanvasViewportRequest request = cli::canvas_viewport_request(
        cli::kViewportMinExtent, cli::kViewportMinExtent, cli::kViewportMinDpr);
    expect(!has_error(request), "the minimum extent and DPR are usable");
    expect(!request.clamped, "the minimum is not a clamp");
    expect_near(request.width, 1.0F, "a 1x1 width is kept");
    expect_near(request.dpi, cli::kViewportMinDpr, "the minimum DPR is kept");
}

// The page needs the numbers, so they are pinned here: a change to one is a change to the other
// side's test too, and that coupling is the point.
void the_limits_are_the_documented_numbers() {
    expect_near(cli::kViewportMaxExtent, 8192.0F, "the extent maximum is 8192");
    expect_near(cli::kViewportMinExtent, 1.0F, "the extent minimum is 1");
    expect_near(cli::kViewportMaxDpr, 4.0F, "the DPR maximum is 4");
    expect_near(cli::kViewportMinDpr, 0.01F, "the DPR minimum is 0.01");
    expect(cli::kViewportMaxBackingBytes == 256ULL * 1024ULL * 1024ULL,
           "the backing-store cap is 256 MiB");
}

} // namespace

int main() {
    an_ordinary_request_is_unchanged();
    the_limits_themselves_are_legal();
    a_huge_extent_clamps_instead_of_failing();
    a_high_dpr_clamps_the_scale_only();
    the_byte_cap_reduces_the_dpr();
    zero_and_negative_are_refused();
    non_finite_values_are_refused();
    the_minimum_is_legal();
    the_limits_are_the_documented_numbers();
    if (failures == 0) {
        std::cout << "canvas_viewport_limit: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
