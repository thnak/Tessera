// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

namespace tessera::ui::style {

struct ColorRgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct ColorParseResult {
    ColorRgba color;
    bool ok = false;
};

// The resolved `color-scheme` a colour value is evaluated against. `light-dark()` is the only
// function that reads it (CSS Color 5 section 3): every other colour is scheme-independent, so a
// caller that has no scheme (a test stub, a theme generator) keeps the light branch by default.
struct ColorContext {
    bool dark = false;
};

// Parses one CSS colour: named, `#rgb[a]`/`#rrggbb[aa]`, `rgb()`/`rgba()`, `hsl()`/`hsla()`,
// `transparent`, `currentcolor` is NOT handled here (it is a keyword the cascade resolves),
// `light-dark()` and `color-mix()`. `light-dark()` picks its branch from `context`; `color-mix()`
// interpolates in the requested space. A nested function is evaluated recursively, so
// `color-mix(in oklab, light-dark(#fff, #000) 40%, red)` works.
ColorParseResult parse_color(std::string_view text, const ColorContext& context = {});
std::string format_color(const ColorRgba& color);
ColorRgba interpolate_color(const ColorRgba& from, const ColorRgba& to, float t);

} // namespace tessera::ui::style
