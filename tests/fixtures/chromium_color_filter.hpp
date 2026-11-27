// SPDX-License-Identifier: MIT
//
// Chromium measurements of the CSS colour filter functions (issue #617), the evidence the style
// parser, the reference formulas (gpu/color_filter.hpp), the CPU twin and the GPU pass are held to.
//
// Method (Chromium 158 via Playwright, Windows, device pixel ratio 1): each sample is an 8x8 px
// `<div>` with a background colour and `filter: <css>`, screenshotted, then decoded and read at its
// centre. Opaque samples sit over #ff00ff (no function here changes alpha, so the backdrop never
// shows). Alpha samples were measured twice, over white and over black; the black capture is the
// premultiplied colour P = colour * alpha * opacity, and the pair also gave the output alpha
// (A = 1 - (white - black)), which matches input alpha * opacity() to 1/255. All 530 opaque and 36
// alpha samples of the full sweep agree with the reference formulas within 1/255 when every
// function clamps its result to [0, 1] before the next one runs; folding a chain into one matrix is
// off by up to 118/255 (`brightness(2) contrast(0.5)`: 191 vs 255), so Chromium clamps per
// function. `CSS.supports('filter', value)` and `getComputedStyle(el).filter` supplied the validity
// and computed-value rows.

#pragma once

#include <array>

namespace tessera::fixtures::chromium_filter {

struct OpaqueSample {
    const char* css;
    std::array<int, 3> input;  // 0..255 background colour
    std::array<int, 3> output; // Chromium's pixel
};

struct AlphaSample {
    const char* css;
    std::array<int, 3> input;
    float alpha;
    std::array<int, 3> over_black; // premultiplied output colour
};

struct ValiditySample {
    const char* css; // the value of `filter`
    bool valid;      // CSS.supports('filter', css)
};

struct ComputedSample {
    const char* css;
    const char* computed; // getComputedStyle(el).filter
};

inline constexpr OpaqueSample kOpaque[] = {
    {"brightness(0)", {128, 64, 200}, {0, 0, 0}},
    {"brightness(0)", {10, 200, 90}, {0, 0, 0}},
    {"brightness(0)", {255, 0, 0}, {0, 0, 0}},
    {"brightness(0.5)", {128, 64, 200}, {64, 32, 100}},
    {"brightness(0.5)", {10, 200, 90}, {5, 100, 45}},
    {"brightness(0.5)", {255, 0, 0}, {128, 0, 0}},
    {"brightness(1.5)", {128, 64, 200}, {192, 96, 255}},
    {"brightness(1.5)", {10, 200, 90}, {15, 255, 135}},
    {"brightness(1.5)", {255, 0, 0}, {255, 0, 0}},
    {"brightness(2)", {128, 64, 200}, {255, 128, 255}},
    {"brightness(2)", {10, 200, 90}, {20, 255, 180}},
    {"brightness(2)", {255, 0, 0}, {255, 0, 0}},
    {"brightness(150%)", {128, 64, 200}, {192, 96, 255}},
    {"brightness(150%)", {10, 200, 90}, {15, 255, 135}},
    {"brightness(150%)", {255, 0, 0}, {255, 0, 0}},
    {"brightness()", {128, 64, 200}, {128, 64, 200}},
    {"brightness()", {10, 200, 90}, {10, 200, 90}},
    {"brightness()", {255, 0, 0}, {255, 0, 0}},
    {"contrast(0)", {128, 64, 200}, {128, 128, 128}},
    {"contrast(0)", {10, 200, 90}, {128, 128, 128}},
    {"contrast(0)", {255, 0, 0}, {128, 128, 128}},
    {"contrast(0.5)", {128, 64, 200}, {128, 96, 164}},
    {"contrast(0.5)", {10, 200, 90}, {69, 164, 109}},
    {"contrast(0.5)", {255, 0, 0}, {191, 64, 64}},
    {"contrast(2)", {128, 64, 200}, {128, 0, 255}},
    {"contrast(2)", {10, 200, 90}, {0, 255, 52}},
    {"contrast(2)", {255, 0, 0}, {255, 0, 0}},
    {"contrast(200%)", {128, 64, 200}, {128, 0, 255}},
    {"contrast(200%)", {10, 200, 90}, {0, 255, 52}},
    {"contrast(200%)", {255, 0, 0}, {255, 0, 0}},
    {"grayscale(0.5)", {128, 64, 200}, {108, 76, 144}},
    {"grayscale(0.5)", {10, 200, 90}, {81, 176, 121}},
    {"grayscale(0.5)", {255, 0, 0}, {155, 27, 27}},
    {"grayscale(1)", {128, 64, 200}, {87, 87, 87}},
    {"grayscale(1)", {10, 200, 90}, {152, 152, 152}},
    {"grayscale(1)", {255, 0, 0}, {54, 54, 54}},
    {"grayscale(2)", {128, 64, 200}, {87, 87, 87}},
    {"grayscale(2)", {10, 200, 90}, {152, 152, 152}},
    {"grayscale(2)", {255, 0, 0}, {54, 54, 54}},
    {"grayscale(150%)", {128, 64, 200}, {87, 87, 87}},
    {"grayscale(150%)", {10, 200, 90}, {152, 152, 152}},
    {"grayscale(150%)", {255, 0, 0}, {54, 54, 54}},
    {"grayscale()", {128, 64, 200}, {87, 87, 87}},
    {"grayscale()", {10, 200, 90}, {152, 152, 152}},
    {"grayscale()", {255, 0, 0}, {54, 54, 54}},
    {"sepia(0.5)", {128, 64, 200}, {133, 93, 148}},
    {"sepia(0.5)", {10, 200, 90}, {92, 178, 106}},
    {"sepia(0.5)", {255, 0, 0}, {178, 44, 35}},
    {"sepia(1)", {128, 64, 200}, {137, 122, 95}},
    {"sepia(1)", {10, 200, 90}, {175, 156, 121}},
    {"sepia(1)", {255, 0, 0}, {100, 89, 69}},
    {"sepia(2)", {128, 64, 200}, {137, 122, 95}},
    {"sepia(2)", {10, 200, 90}, {175, 156, 121}},
    {"sepia(2)", {255, 0, 0}, {100, 89, 69}},
    {"sepia()", {128, 64, 200}, {137, 122, 95}},
    {"sepia()", {10, 200, 90}, {175, 156, 121}},
    {"sepia()", {255, 0, 0}, {100, 89, 69}},
    {"saturate(0)", {128, 64, 200}, {87, 87, 87}},
    {"saturate(0)", {10, 200, 90}, {152, 152, 152}},
    {"saturate(0)", {255, 0, 0}, {54, 54, 54}},
    {"saturate(0.5)", {128, 64, 200}, {108, 76, 144}},
    {"saturate(0.5)", {10, 200, 90}, {81, 176, 121}},
    {"saturate(0.5)", {255, 0, 0}, {155, 27, 27}},
    {"saturate(2)", {128, 64, 200}, {168, 41, 255}},
    {"saturate(2)", {10, 200, 90}, {0, 248, 28}},
    {"saturate(2)", {255, 0, 0}, {255, 0, 0}},
    {"saturate(5)", {128, 64, 200}, {255, 0, 255}},
    {"saturate(5)", {10, 200, 90}, {0, 255, 0}},
    {"saturate(5)", {255, 0, 0}, {255, 0, 0}},
    {"saturate(200%)", {128, 64, 200}, {168, 41, 255}},
    {"saturate(200%)", {10, 200, 90}, {0, 248, 28}},
    {"saturate(200%)", {255, 0, 0}, {255, 0, 0}},
    {"hue-rotate(30deg)", {128, 64, 200}, {179, 52, 165}},
    {"hue-rotate(30deg)", {10, 200, 90}, {0, 195, 169}},
    {"hue-rotate(30deg)", {255, 0, 0}, {201, 25, 0}},
    {"hue-rotate(90deg)", {128, 64, 200}, {200, 58, 47}},
    {"hue-rotate(90deg)", {10, 200, 90}, {90, 156, 255}},
    {"hue-rotate(90deg)", {255, 0, 0}, {0, 91, 0}},
    {"hue-rotate(180deg)", {128, 64, 200}, {47, 111, 0}},
    {"hue-rotate(180deg)", {10, 200, 90}, {255, 103, 213}},
    {"hue-rotate(180deg)", {255, 0, 0}, {0, 109, 109}},
    {"hue-rotate(1turn)", {128, 64, 200}, {128, 64, 200}},
    {"hue-rotate(1turn)", {10, 200, 90}, {10, 200, 90}},
    {"hue-rotate(1turn)", {255, 0, 0}, {255, 0, 0}},
    {"hue-rotate(1rad)", {128, 64, 200}, {204, 50, 114}},
    {"hue-rotate(1rad)", {10, 200, 90}, {23, 181, 237}},
    {"hue-rotate(1rad)", {255, 0, 0}, {117, 56, 0}},
    {"hue-rotate(100grad)", {128, 64, 200}, {200, 58, 47}},
    {"hue-rotate(100grad)", {10, 200, 90}, {90, 156, 255}},
    {"hue-rotate(100grad)", {255, 0, 0}, {0, 91, 0}},
    {"hue-rotate(-90deg)", {128, 64, 200}, {0, 117, 128}},
    {"hue-rotate(-90deg)", {10, 200, 90}, {213, 148, 10}},
    {"hue-rotate(-90deg)", {255, 0, 0}, {109, 18, 255}},
    {"hue-rotate(0)", {128, 64, 200}, {128, 64, 200}},
    {"hue-rotate(0)", {10, 200, 90}, {10, 200, 90}},
    {"hue-rotate(0)", {255, 0, 0}, {255, 0, 0}},
    {"hue-rotate()", {128, 64, 200}, {128, 64, 200}},
    {"hue-rotate()", {10, 200, 90}, {10, 200, 90}},
    {"hue-rotate()", {255, 0, 0}, {255, 0, 0}},
    {"invert(0.5)", {128, 64, 200}, {128, 128, 128}},
    {"invert(0.5)", {10, 200, 90}, {128, 128, 128}},
    {"invert(0.5)", {255, 0, 0}, {128, 128, 128}},
    {"invert(1)", {128, 64, 200}, {127, 191, 55}},
    {"invert(1)", {10, 200, 90}, {245, 55, 165}},
    {"invert(1)", {255, 0, 0}, {0, 255, 255}},
    {"invert(2)", {128, 64, 200}, {127, 191, 55}},
    {"invert(2)", {10, 200, 90}, {245, 55, 165}},
    {"invert(2)", {255, 0, 0}, {0, 255, 255}},
    {"invert()", {128, 64, 200}, {127, 191, 55}},
    {"invert()", {10, 200, 90}, {245, 55, 165}},
    {"invert()", {255, 0, 0}, {0, 255, 255}},
    {"brightness(2) contrast(0.5)", {128, 64, 200}, {191, 128, 191}},
    {"brightness(2) contrast(0.5)", {10, 200, 90}, {74, 191, 154}},
    {"brightness(2) contrast(0.5)", {255, 0, 0}, {191, 64, 64}},
    {"contrast(0.5) brightness(2)", {128, 64, 200}, {255, 191, 255}},
    {"contrast(0.5) brightness(2)", {10, 200, 90}, {137, 255, 217}},
    {"contrast(0.5) brightness(2)", {255, 0, 0}, {255, 128, 128}},
    {"brightness(3) invert(1)", {128, 64, 200}, {0, 63, 0}},
    {"brightness(3) invert(1)", {10, 200, 90}, {225, 0, 0}},
    {"brightness(3) invert(1)", {255, 0, 0}, {0, 255, 255}},
    {"invert(1) brightness(3)", {128, 64, 200}, {255, 255, 165}},
    {"invert(1) brightness(3)", {10, 200, 90}, {255, 165, 255}},
    {"invert(1) brightness(3)", {255, 0, 0}, {0, 255, 255}},
    {"saturate(4) grayscale(0.5)", {128, 64, 200}, {160, 36, 163}},
    {"saturate(4) grayscale(0.5)", {10, 200, 90}, {91, 219, 91}},
    {"saturate(4) grayscale(0.5)", {255, 0, 0}, {155, 27, 27}},
    {"grayscale(0.5) saturate(4)", {128, 64, 200}, {168, 41, 255}},
    {"grayscale(0.5) saturate(4)", {10, 200, 90}, {0, 248, 28}},
    {"grayscale(0.5) saturate(4)", {255, 0, 0}, {255, 0, 0}},
    {"sepia(1) hue-rotate(90deg)", {128, 64, 200}, {95, 133, 110}},
    {"sepia(1) hue-rotate(90deg)", {10, 200, 90}, {121, 170, 140}},
    {"sepia(1) hue-rotate(90deg)", {255, 0, 0}, {69, 97, 80}},
    {"hue-rotate(90deg) sepia(1)", {128, 64, 200}, {132, 117, 92}},
    {"hue-rotate(90deg) sepia(1)", {10, 200, 90}, {203, 181, 141}},
    {"hue-rotate(90deg) sepia(1)", {255, 0, 0}, {70, 62, 48}},
    {"contrast(3) contrast(3)", {128, 64, 200}, {132, 0, 255}},
    {"contrast(3) contrast(3)", {10, 200, 90}, {0, 255, 0}},
    {"contrast(3) contrast(3)", {255, 0, 0}, {255, 0, 0}},
    {"brightness(0.5) brightness(0.5)", {128, 64, 200}, {32, 16, 50}},
    {"brightness(0.5) brightness(0.5)", {10, 200, 90}, {2, 50, 22}},
    {"brightness(0.5) brightness(0.5)", {255, 0, 0}, {64, 0, 0}},
};

inline constexpr AlphaSample kAlpha[] = {
    {"brightness(2)", {200, 100, 50}, 0.5f, {128, 100, 50}},
    {"brightness(2)", {255, 0, 0}, 0.25f, {64, 0, 0}},
    {"brightness(2)", {40, 160, 220}, 0.8f, {64, 204, 204}},
    {"brightness(0.5)", {200, 100, 50}, 0.5f, {50, 25, 12}},
    {"brightness(0.5)", {255, 0, 0}, 0.25f, {32, 0, 0}},
    {"brightness(0.5)", {40, 160, 220}, 0.8f, {16, 64, 88}},
    {"contrast(2)", {200, 100, 50}, 0.5f, {128, 36, 0}},
    {"contrast(2)", {255, 0, 0}, 0.25f, {64, 0, 0}},
    {"contrast(2)", {40, 160, 220}, 0.8f, {0, 154, 204}},
    {"contrast(0.5)", {200, 100, 50}, 0.5f, {82, 57, 44}},
    {"contrast(0.5)", {255, 0, 0}, 0.25f, {48, 16, 16}},
    {"contrast(0.5)", {40, 160, 220}, 0.8f, {67, 115, 139}},
    {"invert(1)", {200, 100, 50}, 0.5f, {28, 78, 103}},
    {"invert(1)", {255, 0, 0}, 0.25f, {0, 64, 64}},
    {"invert(1)", {40, 160, 220}, 0.8f, {172, 76, 28}},
    {"grayscale(1)", {200, 100, 50}, 0.5f, {59, 59, 59}},
    {"grayscale(1)", {255, 0, 0}, 0.25f, {14, 14, 14}},
    {"grayscale(1)", {40, 160, 220}, 0.8f, {111, 111, 111}},
    {"sepia(1)", {200, 100, 50}, 0.5f, {82, 73, 57}},
    {"sepia(1)", {255, 0, 0}, 0.25f, {25, 22, 17}},
    {"sepia(1)", {40, 160, 220}, 0.8f, {144, 128, 100}},
    {"saturate(3)", {200, 100, 50}, 0.5f, {128, 32, 0}},
    {"saturate(3)", {255, 0, 0}, 0.25f, {64, 0, 0}},
    {"saturate(3)", {40, 160, 220}, 0.8f, {0, 162, 204}},
    {"hue-rotate(90deg)", {200, 100, 50}, 0.5f, {25, 73, 18}},
    {"hue-rotate(90deg)", {255, 0, 0}, 0.25f, {0, 23, 0}},
    {"hue-rotate(90deg)", {40, 160, 220}, 0.8f, {176, 84, 190}},
    {"opacity(0.5)", {200, 100, 50}, 0.5f, {50, 25, 12}},
    {"opacity(0.5)", {255, 0, 0}, 0.25f, {32, 0, 0}},
    {"opacity(0.5)", {40, 160, 220}, 0.8f, {16, 64, 88}},
    {"brightness(2) contrast(2)", {200, 100, 50}, 0.5f, {128, 128, 36}},
    {"brightness(2) contrast(2)", {255, 0, 0}, 0.25f, {64, 0, 0}},
    {"brightness(2) contrast(2)", {40, 160, 220}, 0.8f, {26, 204, 204}},
};

inline constexpr ValiditySample kValidity[] = {
    {"brightness(-1)", false},
    {"contrast(-1)", false},
    {"saturate(-1)", false},
    {"opacity(-1)", false},
    {"grayscale(-0.1)", false},
    {"sepia(-1)", false},
    {"invert(-0.5)", false},
    {"hue-rotate(10)", false},
    {"hue-rotate(0)", true},
    {"hue-rotate(10px)", false},
    {"hue-rotate(-10deg)", true},
    {"hue-rotate(10DEG)", true},
    {"brightness(1 2)", false},
    {"brightness(1px)", false},
    {"brightness(1%)", true},
    {"brightness(+.5)", true},
    {"brightness(1e0)", true},
    {"brightness(calc(1 + 1))", true},
    {"brightness(calc(100% + 10%))", true},
    {"grayscale(0.5 0.5)", false},
    {"blur(2px) brightness(1.2)", true},
    {"brightness(1.2),contrast(2)", false},
    {"BRIGHTNESS(2)", true},
    {"brightness( 2 )", true},
    {"brightness(none)", false},
    {"hue-rotate(1.5turn)", true},
    {"invert(100%)", true},
    {"invert(250%)", true},
    {"opacity(1e3)", true},
    {"hue-rotate(0deg)", true},
    {"hue-rotate(.5rad)", true},
    {"saturate(0%)", true},
};

inline constexpr ComputedSample kComputed[] = {
    {"brightness()", "brightness(1)"},
    {"hue-rotate()", "hue-rotate(0deg)"},
    {"invert(250%)", "invert(1)"},
    {"brightness(150%)", "brightness(1.5)"},
    {"hue-rotate(1turn)", "hue-rotate(360deg)"},
    {"grayscale(2)", "grayscale(1)"},
    {"opacity(50%)", "opacity(0.5)"},
    {"hue-rotate(0)", "hue-rotate(0deg)"},
    {"hue-rotate(100grad)", "hue-rotate(90deg)"},
    {"hue-rotate(1rad)", "hue-rotate(57.2958deg)"},
    {"brightness(calc(1 + 1))", "brightness(2)"},
};

} // namespace tessera::fixtures::chromium_filter
