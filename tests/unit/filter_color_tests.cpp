// SPDX-License-Identifier: MIT
//
// The CSS colour filter functions of `filter` (issue #617): brightness, contrast, grayscale, sepia,
// saturate, hue-rotate, invert and opacity. Every expectation is a Chromium measurement
// (tests/fixtures/chromium_color_filter.hpp), not a reading of the spec:
//  - the grammar: which arguments are valid (negatives, percentages, angle units, the bare 0, no
//    argument), checked against `CSS.supports('filter', ...)`, and the computed values (clamping
//    of grayscale/sepia/invert/opacity at 1, angles in degrees);
//  - the reference formulas (gpu/color_filter.hpp) reproduce Chromium's pixels within 1/255 for
//    opaque and translucent inputs, clamping after every function of a list;
//  - the paint pass packs adjacent colour functions three to an op, keeps order against blur() and
//    drop-shadow(), converts hue-rotate to radians, and adds no halo.

#include "chromium_color_filter.hpp"
#include "paint_filter.hpp"
#include "tessera/rendering/gpu/color_filter.hpp"
#include "tessera/ui/style/filter_value.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace chromium = tessera::fixtures::chromium_filter;
using tessera::rendering::gpu::ColorFunction;
using tessera::rendering::gpu::LayerFilterChain;
using tessera::rendering::gpu::LayerFilterOp;
using tessera::rendering::gpu::StraightPixel;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Filter;
using tessera::ui::style::FilterFunction;
using tessera::ui::style::parse_filter;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "filter_color: " << message << "\n";
    }
    return condition;
}

bool close(float a, float b, float tolerance = 1.0e-4f) {
    return std::abs(a - b) <= tolerance;
}

bool starts_with(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

// The one place this engine knowingly differs from Chromium: `calc()` is not part of the V1 value
// grammar anywhere, so a function whose argument is a calc() is rejected.
bool is_calc(const std::string& css) {
    return css.find("calc(") != std::string::npos;
}

bool validity_matches_chromium() {
    bool ok = true;
    for (const chromium::ValiditySample& sample : chromium::kValidity) {
        const bool ours = parse_filter(sample.css).ok();
        const bool expected = is_calc(sample.css) ? false : sample.valid;
        ok =
            expect(ours == expected, std::string("validity of ") + sample.css + ": Chromium says " +
                                         (sample.valid ? "valid" : "invalid")) &&
            ok;
    }
    return ok;
}

// What Chromium serializes for `filter` (getComputedStyle) is the value this parser stores, for
// every row that is not an identity (an identity is dropped, so it paints as `none`).
bool computed_values_match_chromium() {
    bool ok = true;
    for (const chromium::ComputedSample& sample : chromium::kComputed) {
        if (is_calc(sample.css)) {
            continue;
        }
        const std::string computed = sample.computed;
        const auto parsed = parse_filter(sample.css);
        const bool identity = computed == "brightness(1)" || computed == "hue-rotate(0deg)" ||
                              computed == "hue-rotate(360deg)";
        if (identity) {
            ok = expect(parsed.ok() && parsed.filter.none(),
                        std::string("an identity is dropped: ") + sample.css) &&
                 ok;
            continue;
        }
        if (!expect(parsed.ok() && parsed.filter.count == 1,
                    std::string("parses: ") + sample.css)) {
            ok = false;
            continue;
        }
        const float expected = std::strtof(computed.c_str() + computed.find('(') + 1, nullptr);
        ok = expect(close(parsed.filter.functions[0].amount, expected, 1.0e-3f),
                    std::string("computed value of ") + sample.css + " is " + computed) &&
             ok;
    }
    return ok;
}

bool identities_and_limits() {
    bool ok = true;
    for (const char* css :
         {"brightness(1)", "brightness()", "contrast()", "saturate()", "opacity()",
          "contrast(100%)", "saturate(1)", "opacity(1)", "grayscale(0)", "sepia(0%)", "invert(0)",
          "hue-rotate(0)", "hue-rotate(360deg)", "hue-rotate(1turn)", "hue-rotate(-720deg)",
          "brightness(1) blur(0) opacity(100%)"}) {
        const auto parsed = parse_filter(css);
        ok = expect(parsed.ok() && parsed.filter.none(), std::string("identity drops: ") + css) &&
             ok;
    }
    // An omitted argument is 1: grayscale(), sepia() and invert() are the full effect.
    for (const char* css : {"grayscale()", "sepia()", "invert()"}) {
        const auto parsed = parse_filter(css);
        ok = expect(parsed.ok() && parsed.filter.count == 1 &&
                        parsed.filter.functions[0].amount == 1.0f,
                    std::string("the default amount of ") + css + " is 1") &&
             ok;
    }
    // invert(50%) is a flat grey, not an identity; hue-rotate keeps a partial turn.
    ok = expect(parse_filter("invert(50%)").filter.count == 1, "invert(50%) stays") && ok;
    const auto hue = parse_filter("hue-rotate(100grad)");
    ok = expect(hue.ok() && close(hue.filter.functions[0].amount, 90.0f, 1.0e-3f), "grad") && ok;
    // Nine functions are three runs of three, so they fit; a tenth, or four steps, do not.
    std::string nine;
    for (int index = 0; index < 9; ++index) {
        nine += "brightness(2) ";
    }
    const auto full = parse_filter(nine);
    ok = expect(full.ok() && full.filter.count == 9 && full.filter.op_count() == 3, "nine fit") &&
         ok;
    ok = expect(!parse_filter(nine + "contrast(2)").ok(), "ten functions are too many") && ok;
    ok = expect(!parse_filter("blur(1px) brightness(2) blur(1px) brightness(2)").ok(),
                "four steps are too many") &&
         ok;
    ok = expect(parse_filter("brightness(2) brightness(2) brightness(2) blur(1px) sepia(1)").ok(),
                "a run of three, a blur and a sepia fit") &&
         ok;
    const auto error = parse_filter("grayscale(-1)");
    ok = expect(!error.ok() && error.token == "grayscale", "the diagnostic names the function") &&
         ok;
    return ok;
}

// The chain the paint pass hands the renderer for `css`.
LayerFilterChain chain_of(const char* css) {
    ComputedStyle style;
    style.set("filter", css);
    return tessera::ui::paint::detail::layer_filter_chain(style, tessera::rendering::rhi::Color{},
                                                          tessera::rendering::gpu::GlassLimits{});
}

// Runs the colour ops of `chain` over a straight-alpha pixel, as the shader and the CPU twin do.
StraightPixel run(const LayerFilterChain& chain, float r, float g, float b, float a) {
    StraightPixel pixel{r, g, b, a};
    for (std::size_t index = 0; index < chain.count; ++index) {
        if (chain.ops[index].kind == LayerFilterOp::Kind::ColorFunctions) {
            tessera::rendering::gpu::apply_color_functions(chain.ops[index], pixel);
        }
    }
    return pixel;
}

bool reference_matches_chromium() {
    bool ok = true;
    for (const chromium::OpaqueSample& sample : chromium::kOpaque) {
        const StraightPixel out =
            run(chain_of(sample.css), static_cast<float>(sample.input[0]) / 255.0f,
                static_cast<float>(sample.input[1]) / 255.0f,
                static_cast<float>(sample.input[2]) / 255.0f, 1.0f);
        const float got[3] = {out.r, out.g, out.b};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float expected = static_cast<float>(sample.output[channel]);
            ok =
                expect(std::abs(got[channel] * 255.0f - expected) <= 1.0f,
                       std::string(sample.css) + " on (" + std::to_string(sample.input[0]) + "," +
                           std::to_string(sample.input[1]) + "," + std::to_string(sample.input[2]) +
                           ") channel " + std::to_string(channel) + ": got " +
                           std::to_string(got[channel] * 255.0f) + ", Chromium " +
                           std::to_string(sample.output[channel])) &&
                ok;
        }
    }
    for (const chromium::AlphaSample& sample : chromium::kAlpha) {
        const StraightPixel out =
            run(chain_of(sample.css), static_cast<float>(sample.input[0]) / 255.0f,
                static_cast<float>(sample.input[1]) / 255.0f,
                static_cast<float>(sample.input[2]) / 255.0f, sample.alpha);
        // The measurement is the premultiplied colour over black.
        const float got[3] = {out.r * out.a, out.g * out.a, out.b * out.a};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            ok = expect(std::abs(got[channel] * 255.0f -
                                 static_cast<float>(sample.over_black[channel])) <= 1.0f,
                        std::string(sample.css) + " at alpha " + std::to_string(sample.alpha) +
                            " channel " + std::to_string(channel) + ": got " +
                            std::to_string(got[channel] * 255.0f) + ", Chromium " +
                            std::to_string(sample.over_black[channel])) &&
                 ok;
        }
    }
    // A list is not one folded matrix: brightness(2) contrast(0.5) on white is 191, not 255
    // (measured: each function clamps before the next).
    const StraightPixel white =
        run(chain_of("brightness(2) contrast(0.5)"), 1.0f, 1.0f, 1.0f, 1.0f);
    ok =
        expect(std::abs(white.r * 255.0f - 191.0f) <= 1.0f, "per-function clamping on white") && ok;
    return ok;
}

bool packing() {
    bool ok = true;
    const LayerFilterChain runs =
        chain_of("brightness(1.5) contrast(1.2) saturate(1.1) hue-rotate(90deg)");
    ok = expect(runs.count == 2 && runs.ops[0].color_count() == 3 && runs.ops[1].color_count() == 1,
                "three colour functions share an op, the fourth starts the next") &&
         ok;
    ok = expect(runs.ops[0].color_function(0) == ColorFunction::Brightness &&
                    close(runs.ops[0].color_amount(0), 1.5f) &&
                    runs.ops[0].color_function(2) == ColorFunction::Saturate &&
                    close(runs.ops[0].color_amount(2), 1.1f) &&
                    runs.ops[1].color_function(0) == ColorFunction::HueRotate &&
                    close(runs.ops[1].color_amount(0), 3.14159265f / 2.0f, 1.0e-5f),
                "order, amounts, and hue-rotate in radians") &&
         ok;
    const LayerFilterChain mixed = chain_of("sepia(1) blur(2px) invert(1)");
    ok = expect(mixed.count == 3, "a blur splits the runs") && ok;
    ok = expect(mixed.ops[0].kind == LayerFilterOp::Kind::ColorFunctions &&
                    mixed.ops[1].kind == LayerFilterOp::Kind::Blur &&
                    mixed.ops[2].kind == LayerFilterOp::Kind::ColorFunctions,
                "the list order survives") &&
         ok;
    // Colour functions move no pixel: no halo, no reach, and the blur's is the only one.
    ComputedStyle colour_only;
    colour_only.set("filter", "brightness(1.2) grayscale(0.5)");
    const auto outsets = tessera::ui::paint::detail::filter_outsets(
        colour_only, tessera::rendering::gpu::GlassLimits{});
    ok = expect(outsets.left == 0.0f && outsets.right == 0.0f && outsets.top == 0.0f &&
                    outsets.bottom == 0.0f && !outsets.blurred,
                "a colour function adds no halo") &&
         ok;
    LayerFilterOp op;
    op.set_color_function(1, ColorFunction::Invert, 0.25f);
    ok = expect(op.color_count() == 0, "a run ends at the first None") && ok;
    op.set_color_function(0, ColorFunction::Opacity, 0.5f);
    ok = expect(op.color_count() == 2 && op.color_function(1) == ColorFunction::Invert &&
                    op.color_amount(1) == 0.25f && op.color_amount(0) == 0.5f,
                "slots are independent") &&
         ok;
    return ok;
}

} // namespace

int main() {
    const bool ok = validity_matches_chromium() && computed_values_match_chromium() &&
                    identities_and_limits() && reference_matches_chromium() && packing();
    if (!ok) {
        std::cerr << "filter_color: failed\n";
        return 1;
    }
    std::cout << "filter_color: ok\n";
    return 0;
}
