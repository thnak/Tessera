// SPDX-License-Identifier: MIT

// The text coverage-contrast LUT (display-aware-rendering.md section 3.5, adopted as a D2
// amendment): the table is monotonic, flat at 1.0 through the mid point (dark text gets no boost),
// reaches exactly 1.4 at white, and its index function maps encoded lightness onto 0..255 without
// ever leaving the table. Pure CPU: the header is internal to the text renderer.

#include "text_contrast_lut.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>

namespace {

using tessera::ui::text::detail::contrast_boost_for_index;
using tessera::ui::text::detail::contrast_lut_index;
using tessera::ui::text::detail::kContrastMaxBoost;
using tessera::ui::text::detail::kContrastMidIndex;
using tessera::ui::text::detail::kCoverageContrastLut;
using tessera::ui::text::detail::kCoverageContrastLutSize;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_contrast_lut: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    bool ok = true;

    // Shape: one entry per encoded lightness value, the value being the curve's. The comparison is
    // tolerant on purpose: the table is folded at compile time and the call below is a runtime
    // evaluation, and a compiler that contracts `1.0f + 0.4f * t` into an FMA (GCC's default
    // `-ffp-contract=fast`) may differ from the folded value by one ULP. An exact equality here
    // tests the compiler's floating-point mode, not the curve.
    ok = expect(kCoverageContrastLut.size() == kCoverageContrastLutSize,
                "the table has 256 entries") &&
         ok;
    for (std::size_t index = 0; index < kCoverageContrastLut.size(); ++index) {
        if (std::abs(kCoverageContrastLut[index] - contrast_boost_for_index(index)) > 1e-6f) {
            ok = expect(false, "every entry is the documented curve") && ok;
            break;
        }
    }

    // Endpoints and the flat half: no boost at or below mid encoded lightness, exactly 1.4 at
    // white.
    ok = expect(kCoverageContrastLut.front() == 1.0f, "black gets no boost") && ok;
    ok = expect(kCoverageContrastLut[kContrastMidIndex] == 1.0f, "the mid point gets no boost") &&
         ok;
    for (std::size_t index = 0; index < kContrastMidIndex; ++index) {
        if (kCoverageContrastLut[index] != 1.0f) {
            ok = expect(false, "no boost below mid lightness") && ok;
            break;
        }
    }
    ok = expect(kCoverageContrastLut.back() == kContrastMaxBoost, "white gets the maximum boost") &&
         ok;
    ok = expect(kContrastMaxBoost == 1.4f, "the maximum boost is 1.4") && ok;

    // Monotonic: never decreasing, and strictly increasing after the flat half.
    for (std::size_t index = 0; index + 1 < kCoverageContrastLut.size(); ++index) {
        if (!(kCoverageContrastLut[index + 1] >= kCoverageContrastLut[index])) {
            ok = expect(false, "the table is monotonic") && ok;
            break;
        }
        if (index >= kContrastMidIndex &&
            !(kCoverageContrastLut[index + 1] > kCoverageContrastLut[index])) {
            ok = expect(false, "the table rises above the mid point") && ok;
            break;
        }
    }

    // The index function: encoded lightness in, a valid table index out, never out of bounds.
    ok = expect(contrast_lut_index(0.0f, 0.0f, 0.0f) == 0, "black maps to index 0") && ok;
    ok = expect(contrast_lut_index(1.0f, 1.0f, 1.0f) == 255, "white maps to the last index") && ok;
    ok = expect(contrast_lut_index(0.5f, 0.5f, 0.5f) == kContrastMidIndex,
                "mid grey maps to the mid index") &&
         ok;
    ok = expect(contrast_lut_index(-1.0f, 0.0f, 0.0f) == 0, "a negative channel clamps to black") &&
         ok;
    ok = expect(contrast_lut_index(2.0f, 2.0f, 2.0f) == 255, "an out-of-range channel clamps to "
                                                             "white") &&
         ok;
    ok = expect(contrast_lut_index(std::nanf(""), 0.0f, 0.0f) == 0,
                "a NaN channel falls back to black") &&
         ok;
    ok = expect(kCoverageContrastLut[contrast_lut_index(1.0f, 1.0f, 1.0f)] == 1.4f,
                "a white run boosts to 1.4") &&
         ok;
    ok = expect(kCoverageContrastLut[contrast_lut_index(0.1f, 0.1f, 0.1f)] == 1.0f,
                "a dark run gets no boost") &&
         ok;

    return ok ? 0 : 1;
}
