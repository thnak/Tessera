// SPDX-License-Identifier: MIT
//
// Chromium reference for the layer-mask pixel comparison (issue #617). One scene, rendered by
// Chromium (headless, device pixel ratio 1, read back through a canvas): a 64x64 black page with a
// #00ff00 box at (8,8) 48x48, `mask-image` set to one of three gradients, at opacity 1 and 0.5. The
// values are the green channel at every fourth pixel from 10 to 54 (skipping the conic seam at 32)
// along row 32 and along column 32 (red and blue are 0 everywhere). Measured 2026-11-27 with the
// Playwright Chromium on the dev PC.
//
//   linear  linear-gradient(90deg, rgba(0,0,0,0), #000)
//   radial  radial-gradient(circle, #000, rgba(0,0,0,0))
//   conic   conic-gradient(#000, rgba(0,0,0,0))

#pragma once

#include <array>
#include <cstdint>

namespace layer_mask_reference {

inline constexpr std::array<std::uint32_t, 12> kSampleAt{10, 14, 18, 22, 26, 30,
                                                         34, 38, 42, 46, 50, 54};

struct Series {
    const char* name; // "<shape>@<opacity>"
    std::array<int, 12> row32;
    std::array<int, 12> col32;
};

inline constexpr std::array<Series, 6> kChromium{{
    {"linear@1",
     {13, 35, 56, 77, 98, 120, 141, 162, 183, 204, 226, 247},
     {130, 130, 130, 130, 130, 130, 130, 130, 130, 130, 130, 130}},
    {"linear@0.5",
     {6, 17, 28, 38, 49, 60, 70, 81, 91, 102, 113, 123},
     {65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65}},
    {"radial@1",
     {93, 123, 153, 183, 213, 243, 236, 206, 176, 146, 116, 86},
     {93, 123, 153, 183, 213, 243, 236, 206, 176, 146, 116, 86}},
    {"radial@0.5",
     {46, 61, 76, 91, 106, 121, 118, 103, 88, 73, 58, 43},
     {46, 61, 76, 91, 106, 121, 118, 103, 88, 73, 58, 43}},
    {"conic@1",
     {65, 65, 65, 66, 67, 77, 183, 188, 189, 190, 190, 190},
     {254, 254, 253, 253, 251, 242, 135, 131, 129, 129, 128, 128}},
    {"conic@0.5",
     {32, 32, 32, 33, 33, 38, 91, 94, 94, 95, 95, 95},
     {127, 127, 126, 126, 125, 121, 67, 65, 64, 64, 64, 64}},
}};

} // namespace layer_mask_reference
