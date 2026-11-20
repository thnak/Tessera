// SPDX-License-Identifier: MIT
//
// `light-dark()` (issue #620) and `color-mix()` (issue #621): the two CSS Color 5 colour functions
// that take other colours as arguments. Covers the scheme branch, nesting, the four interpolation
// spaces, percentage resolution, malformed calls, the computed-value-time substitution inside a
// larger value, and the `color-scheme` property the first one reads.

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/property_registry.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::ui::style::ColorContext;
using tessera::ui::style::ColorRgba;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::parse_color;
using tessera::ui::style::PropertyRegistry;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "color_functions: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs, float tolerance = 0.01f) {
    return std::abs(lhs - rhs) <= tolerance;
}

bool color_is(const ColorRgba& color, float r, float g, float b, float a = 1.0f,
              float tolerance = 0.01f) {
    return near(color.r, r, tolerance) && near(color.g, g, tolerance) &&
           near(color.b, b, tolerance) && near(color.a, a, tolerance);
}

ColorRgba parsed(std::string_view text, bool dark = false) {
    const auto result = parse_color(text, ColorContext{dark});
    if (!result.ok) {
        std::cerr << "color_functions: failed to parse " << text << "\n";
    }
    return result.color;
}

bool ok(std::string_view text, bool dark = false) {
    return parse_color(text, ColorContext{dark}).ok;
}

// --- light-dark() ------------------------------------------------------------------------------

bool light_dark_picks_the_scheme_branch() {
    bool good = expect(color_is(parsed("light-dark(#ffffff, #000000)"), 1, 1, 1),
                       "light scheme takes the first branch");
    good = expect(color_is(parsed("light-dark(#ffffff, #000000)", true), 0, 0, 0),
                  "dark scheme takes the second branch") &&
           good;
    // The function name is case-insensitive, like every CSS function.
    good = expect(color_is(parsed("LIGHT-DARK(red, blue)"), 1, 0, 0), "name is case-insensitive") &&
           good;
    // Whitespace around the arguments is not significant.
    good = expect(color_is(parsed("light-dark(  red  ,  blue  )", true), 0, 0, 1),
                  "arguments are trimmed") &&
           good;
    return good;
}

bool light_dark_nests_and_rejects_bad_forms() {
    // A nested colour function in either branch is evaluated with the same context.
    bool good = expect(
        color_is(parsed("light-dark(color-mix(in srgb, red 50%, blue), green)"), 0.5f, 0, 0.5f),
        "a color-mix() branch is evaluated");
    good = expect(color_is(parsed("light-dark(red, color-mix(in srgb, red 50%, blue))", true), 0.5f,
                           0, 0.5f),
                  "the dark branch may hold a color-mix()") &&
           good;
    // Arity, a missing branch, an unparseable branch and trailing junk are all invalid colours.
    good = expect(!ok("light-dark(red)"), "one argument is invalid") && good;
    good = expect(!ok("light-dark(red, blue, green)"), "three arguments are invalid") && good;
    good = expect(!ok("light-dark(red, notacolor)"), "an unparseable branch is invalid") && good;
    good = expect(!ok("light-dark(red, blue) 4px"), "trailing text is not a colour") && good;
    good = expect(!ok("light-dark(red, blue"), "an unclosed call is invalid") && good;
    return good;
}

// --- color-mix() -------------------------------------------------------------------------------

bool color_mix_srgb() {
    // 50/50 red and blue in sRGB is the midpoint of the encoded channels.
    bool good = expect(color_is(parsed("color-mix(in srgb, red, blue)"), 0.5f, 0, 0.5f),
                       "srgb 50/50 red and blue");
    // A percentage on one operand sets its weight; the other takes the remainder. `red 25%` means a
    // quarter red and three quarters blue.
    good = expect(color_is(parsed("color-mix(in srgb, red 25%, blue)"), 0.25f, 0, 0.75f),
                  "a single percentage sets that operand's weight") &&
           good;
    good = expect(color_is(parsed("color-mix(in srgb, red, blue 25%)"), 0.75f, 0, 0.25f),
                  "the percentage may be on the second operand") &&
           good;
    // Two percentages are normalized to sum to 100%, so 30/30 is the same as 50/50.
    good = expect(color_is(parsed("color-mix(in srgb, red 30%, blue 30%)"), 0.5f, 0, 0.5f),
                  "two percentages are normalized") &&
           good;
    // 0% of one operand is the other colour exactly.
    good = expect(color_is(parsed("color-mix(in srgb, red 0%, blue)"), 0, 0, 1),
                  "a zero weight yields the other colour") &&
           good;
    return good;
}

bool color_mix_alpha_is_premultiplied() {
    // Mixing with `transparent` keeps the visible colour and halves the alpha, the same rule
    // interpolate_color follows: a plain per-channel blend would pass through dark grey.
    bool good = expect(color_is(parsed("color-mix(in srgb, red, transparent)"), 1, 0, 0, 0.5f),
                       "mixing with transparent keeps the colour");
    good = expect(color_is(parsed("color-mix(in srgb, transparent, white)"), 1, 1, 1, 0.5f),
                  "mixing transparent with white keeps white") &&
           good;
    // Two fully transparent operands stay transparent rather than dividing by zero.
    good = expect(color_is(parsed("color-mix(in srgb, transparent, transparent)"), 0, 0, 0, 0),
                  "two transparent operands stay transparent") &&
           good;
    return good;
}

bool color_mix_spaces() {
    // sRGB-linear interpolates light, so 50/50 black and white is lighter than the encoded midpoint
    // (0.5 encoded is about 0.214 linear, so the linear midpoint encodes to about 0.735).
    const ColorRgba linear = parsed("color-mix(in srgb-linear, black, white)");
    bool good = expect(near(linear.r, 0.7354f, 0.01f),
                       "srgb-linear 50/50 black and white is the linear midpoint");
    good = expect(linear.r > 0.6f, "the linear midpoint is lighter than the encoded one") && good;
    // OKLab and OKLCH both interpolate the rectangular OKLab pair, so they agree with each other.
    const ColorRgba oklab = parsed("color-mix(in oklab, red, blue)");
    const ColorRgba oklch = parsed("color-mix(in oklch, red, blue)");
    good = expect(color_is(oklab, oklch.r, oklch.g, oklch.b, oklch.a, 0.005f),
                  "oklab and oklch agree on a rectangular mix") &&
           good;
    // A perceptual mix of red and blue is not the sRGB midpoint: it keeps more chroma.
    const ColorRgba srgb = parsed("color-mix(in srgb, red, blue)");
    good = expect(!color_is(oklab, srgb.r, srgb.g, srgb.b, srgb.a, 0.02f),
                  "the perceptual mix differs from the encoded one") &&
           good;
    // The space name is case-insensitive and may be padded.
    good = expect(color_is(parsed("color-mix(IN SRGB, red, blue)"), 0.5f, 0, 0.5f),
                  "the space name is case-insensitive") &&
           good;
    return good;
}

bool color_mix_rejects_bad_forms() {
    bool good = expect(!ok("color-mix(red, blue)"), "a missing `in <space>` is invalid");
    good = expect(!ok("color-mix(in hsl, red, blue)"), "an unsupported space is invalid") && good;
    good = expect(!ok("color-mix(in srgb, red)"), "two operands are required") && good;
    good =
        expect(!ok("color-mix(in srgb, red, blue, green)"), "three operands are invalid") && good;
    good = expect(!ok("color-mix(in srgb, red 10% 20%, blue)"), "two percentages on one operand") &&
           good;
    good = expect(!ok("color-mix(in srgb, notacolor, blue)"), "an unparseable operand") && good;
    good = expect(!ok("color-mix(in srgb, red 0%, blue 0%)"), "a zero total weight is invalid") &&
           good;
    return good;
}

// --- computed-value-time substitution ----------------------------------------------------------

// Resolves one declaration through the real cascade, so the test exercises the same path a
// stylesheet takes: registry expansion, inheritance, var(), then the colour functions.
ComputedStyle resolve(const PropertyRegistry& registry, std::string property, std::string value,
                      const ComputedStyle* parent = nullptr) {
    tessera::ui::style::CascadeTable table;
    tessera::ui::style::CascadeDeclaration declaration;
    declaration.property = std::move(property);
    declaration.value = std::move(value);
    table.add(std::move(declaration));
    return registry.compute_with_initial(parent, table);
}

bool substitution_inside_a_larger_value() {
    const PropertyRegistry registry;
    bool good = true;

    // A colour function inside `box-shadow` resolves to a plain colour, so the paint parser sees
    // the same syntax it always did.
    const ComputedStyle shadow =
        resolve(registry, "box-shadow", "0 0 4px light-dark(#000000, #ffffff)");
    const std::string* shadow_value = shadow.find("box-shadow");
    good = expect(shadow_value != nullptr && shadow_value->find("light-dark") == std::string::npos,
                  "light-dark() is substituted inside box-shadow") &&
           good;
    good =
        expect(shadow_value != nullptr && shadow_value->find("rgb(0, 0, 0)") != std::string::npos,
               "the light branch becomes a plain rgb()") &&
        good;

    // A colour property resolves to the colour itself.
    const ComputedStyle background =
        resolve(registry, "background-color", "color-mix(in srgb, red 50%, blue)");
    const std::string* background_value = background.find("background-color");
    good = expect(background_value != nullptr &&
                      background_value->find("color-mix") == std::string::npos,
                  "color-mix() is substituted in background-color") &&
           good;

    // A malformed call drops the declaration, so the initial value applies.
    const ComputedStyle broken = resolve(registry, "background-color", "light-dark(red)");
    const std::string* broken_value = broken.find("background-color");
    good = expect(broken_value != nullptr && *broken_value == "transparent",
                  "a malformed call falls back to the initial value") &&
           good;
    return good;
}

bool color_scheme_selects_the_branch() {
    const PropertyRegistry registry;
    // `color-scheme: dark` on the element makes its own light-dark() take the dark branch.
    tessera::ui::style::CascadeTable table;
    tessera::ui::style::CascadeDeclaration scheme;
    scheme.property = "color-scheme";
    scheme.value = "dark";
    scheme.source_order = 0;
    table.add(std::move(scheme));
    tessera::ui::style::CascadeDeclaration background;
    background.property = "background-color";
    background.value = "light-dark(#ffffff, #000000)";
    background.source_order = 1;
    table.add(std::move(background));
    const ComputedStyle dark = registry.compute_with_initial(nullptr, table);
    const std::string* value = dark.find("background-color");
    bool good = expect(value != nullptr && value->find("rgb(0, 0, 0)") != std::string::npos,
                       "color-scheme: dark selects the dark branch");

    // The property is inherited, so a child of a dark element resolves dark without repeating it.
    tessera::ui::style::CascadeTable child_table;
    tessera::ui::style::CascadeDeclaration child_background;
    child_background.property = "background-color";
    child_background.value = "light-dark(#ffffff, #000000)";
    child_table.add(std::move(child_background));
    const ComputedStyle child = registry.compute_with_initial(&dark, child_table);
    const std::string* child_value = child.find("background-color");
    good = expect(child_value != nullptr && child_value->find("rgb(0, 0, 0)") != std::string::npos,
                  "color-scheme is inherited") &&
           good;

    // `normal` (the initial value) leaves the light branch, which is the documented default.
    const ComputedStyle normal = resolve(registry, "background-color", "light-dark(#ffffff, #000)");
    const std::string* normal_value = normal.find("background-color");
    good = expect(normal_value != nullptr &&
                      normal_value->find("rgb(255, 255, 255)") != std::string::npos,
                  "color-scheme: normal resolves light") &&
           good;
    return good;
}

bool color_scheme_is_registered_and_inherited() {
    const PropertyRegistry registry;
    const auto* info = registry.find("color-scheme");
    bool good = expect(info != nullptr, "color-scheme is a registered property");
    good = expect(info != nullptr && info->inherited, "color-scheme is inherited") && good;
    good = expect(info != nullptr && info->initial == "normal", "color-scheme starts at normal") &&
           good;
    return good;
}

} // namespace

int main() {
    bool good = true;
    good = light_dark_picks_the_scheme_branch() && good;
    good = light_dark_nests_and_rejects_bad_forms() && good;
    good = color_mix_srgb() && good;
    good = color_mix_alpha_is_premultiplied() && good;
    good = color_mix_spaces() && good;
    good = color_mix_rejects_bad_forms() && good;
    good = substitution_inside_a_larger_value() && good;
    good = color_scheme_selects_the_branch() && good;
    good = color_scheme_is_registered_and_inherited() && good;
    if (!good) {
        std::cerr << "color_functions: FAILED\n";
        return 1;
    }
    std::cout << "color_functions: ok\n";
    return 0;
}
