// SPDX-License-Identifier: MIT
//
// Issue #691: the viewport units and the `@media` features that were promised but missing.
//
// Two things are tested here. First, the shared unit table resolves every unit the spec lists -
// `vmin`, `vmax` and the small/large/dynamic family included - and the small/large/dynamic variants
// collapse to the plain value on a viewport without dynamic chrome, which is what a desktop window
// is. Second, the media features `aspect-ratio`, `hover`, `pointer` and the range syntax parse and
// evaluate, including the two-sided form and the feature-on-the-right form.

#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/viewport_units.hpp"

#include <iostream>
#include <string_view>

namespace {

using tessera::ui::style::evaluate;
using tessera::ui::style::MediaContext;
using tessera::ui::style::MediaHover;
using tessera::ui::style::MediaPointer;
using tessera::ui::style::parse_media_query_list;
using tessera::ui::style::parse_viewport_unit;
using tessera::ui::style::resolve_viewport_length;
using tessera::ui::style::ViewportAxis;
using tessera::ui::style::ViewportLength;
using tessera::ui::style::ViewportSize;
using tessera::ui::style::ViewportUnits;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "viewport_units: " << message << "\n";
    }
    return condition;
}

bool matches(std::string_view text, const MediaContext& context) {
    const auto parsed = parse_media_query_list(text);
    return parsed.ok && evaluate(parsed.list, context);
}

bool parses(std::string_view text) {
    return parse_media_query_list(text).ok;
}

// The pixels a unit resolves to on a 1000x500 viewport, so every expected value is a round number.
double pixels(std::string_view unit, double value, const ViewportUnits& viewport) {
    ViewportSize size = ViewportSize::Default;
    ViewportAxis axis = ViewportAxis::Width;
    if (!parse_viewport_unit(unit, size, axis)) {
        return -1.0;
    }
    return resolve_viewport_length(ViewportLength{size, axis, value}, viewport);
}

bool test_unit_table() {
    ViewportUnits viewport;
    viewport.width = 1000.0;
    viewport.height = 500.0;

    bool ok = true;
    // The plain family.
    ok = expect(pixels("vw", 10.0, viewport) == 100.0, "10vw of 1000 is 100") && ok;
    ok = expect(pixels("vh", 10.0, viewport) == 50.0, "10vh of 500 is 50") && ok;
    ok = expect(pixels("vmin", 10.0, viewport) == 50.0, "10vmin is 10% of the smaller axis") && ok;
    ok = expect(pixels("vmax", 10.0, viewport) == 100.0, "10vmax is 10% of the larger axis") && ok;

    // Without dynamic chrome the small/large/dynamic variants are the plain value. That is the
    // correct answer for a desktop window, not a fallback.
    for (const char* unit : {"svw", "lvw", "dvw"}) {
        ok = expect(pixels(unit, 10.0, viewport) == 100.0, "s/l/d width equals vw") && ok;
    }
    for (const char* unit : {"svh", "lvh", "dvh"}) {
        ok = expect(pixels(unit, 10.0, viewport) == 50.0, "s/l/d height equals vh") && ok;
    }
    ok = expect(pixels("svmin", 10.0, viewport) == 50.0, "svmin equals vmin") && ok;
    ok = expect(pixels("dvmax", 10.0, viewport) == 100.0, "dvmax equals vmax") && ok;

    // An unknown unit is not a viewport unit, so a caller falls through to its own table.
    ViewportSize size = ViewportSize::Default;
    ViewportAxis axis = ViewportAxis::Width;
    ok = expect(!parse_viewport_unit("px", size, axis), "px is not a viewport unit") && ok;
    ok = expect(!parse_viewport_unit("vi", size, axis), "vi needs a writing mode") && ok;
    return ok;
}

bool test_dynamic_chrome() {
    // A viewport that reports chrome: the three sizes differ, and each unit picks its own.
    ViewportUnits viewport;
    viewport.width = 1000.0;
    viewport.height = 800.0;
    viewport.has_dynamic_chrome = true;
    viewport.small_height = 700.0;
    viewport.large_height = 800.0;

    bool ok = true;
    ok = expect(pixels("svh", 10.0, viewport) == 70.0, "svh uses the small height") && ok;
    ok = expect(pixels("lvh", 10.0, viewport) == 80.0, "lvh uses the large height") && ok;
    ok = expect(pixels("dvh", 10.0, viewport) == 80.0, "dvh uses the current height") && ok;
    ok = expect(pixels("vh", 10.0, viewport) == 80.0, "vh uses the current height") && ok;
    return ok;
}

bool test_media_viewport_units() {
    MediaContext context;
    context.viewport_width = 1000.0;
    context.viewport_height = 500.0;

    bool ok = true;
    // `@media` now accepts the same unit set layout does. The viewport is 1000x500, so `vmin` is
    // 500 and `vmax` is 1000: 50vmin is 250px and 50vmax is 500px. The false cases use a value
    // above the 1000px width, so the assertion is about the unit resolving at all rather than about
    // the comparison being off by a factor.
    ok = expect(matches("(min-width: 50vmin)", context), "50vmin of 500 is 250") && ok;
    ok = expect(!matches("(min-width: 250vmin)", context), "250vmin of 500 is 1250") && ok;
    ok = expect(matches("(min-width: 50vmax)", context), "50vmax of 1000 is 500") && ok;
    ok = expect(!matches("(min-width: 150vmax)", context), "150vmax of 1000 is 1500") && ok;
    ok = expect(matches("(min-height: 50dvh)", context), "dvh resolves in @media") && ok;
    return ok;
}

bool test_aspect_ratio() {
    MediaContext context;
    context.viewport_width = 1600.0;
    context.viewport_height = 900.0;

    bool ok = true;
    // 1600/900 is exactly 16/9. A float division would leave a residue; the integer comparison
    // does not.
    ok = expect(matches("(aspect-ratio: 16/9)", context), "16/9 matches 1600x900") && ok;
    ok = expect(matches("(aspect-ratio: 16 / 9)", context), "spaces around the slash") && ok;
    ok = expect(!matches("(aspect-ratio: 4/3)", context), "4/3 does not match") && ok;
    ok = expect(matches("(min-aspect-ratio: 4/3)", context), "min-aspect-ratio") && ok;
    ok = expect(!matches("(max-aspect-ratio: 4/3)", context), "max-aspect-ratio false") && ok;
    ok = expect(matches("(aspect-ratio: 1)", context) == false, "1/1 does not match 16/9") && ok;
    // A bare number is n/1.
    context.viewport_width = 500.0;
    context.viewport_height = 500.0;
    ok = expect(matches("(aspect-ratio: 1)", context), "a bare number is n/1") && ok;
    return ok;
}

bool test_hover_and_pointer() {
    MediaContext context;
    bool ok = true;
    // The defaults are a desktop window: a mouse hovers and points finely.
    ok = expect(matches("(hover: hover)", context), "default hovers") && ok;
    ok = expect(!matches("(hover: none)", context), "default is not hover:none") && ok;
    ok = expect(matches("(pointer: fine)", context), "default pointer is fine") && ok;
    ok = expect(!matches("(pointer: coarse)", context), "default is not coarse") && ok;

    // A touch-only device.
    context.hover = MediaHover::None;
    context.pointer = MediaPointer::Coarse;
    context.any_hover = MediaHover::None;
    context.any_pointer = MediaPointer::Coarse;
    ok = expect(matches("(hover: none)", context), "touch does not hover") && ok;
    ok = expect(matches("(pointer: coarse)", context), "touch is coarse") && ok;
    ok = expect(!matches("(pointer: fine)", context), "touch is not fine") && ok;

    // A hybrid laptop: the primary device is a mouse but a touchscreen is present, so `any-*`
    // answers differently from the primary form.
    context.hover = MediaHover::Hover;
    context.pointer = MediaPointer::Fine;
    context.any_hover = MediaHover::None;
    context.any_pointer = MediaPointer::Coarse;
    ok = expect(matches("(hover: hover)", context), "hybrid primary hovers") && ok;
    ok = expect(matches("(any-hover: none)", context), "hybrid any-hover is none") && ok;
    ok = expect(matches("(any-pointer: coarse)", context), "hybrid any-pointer is coarse") && ok;
    return ok;
}

bool test_range_syntax() {
    MediaContext context;
    context.viewport_width = 800.0;
    context.viewport_height = 600.0;

    bool ok = true;
    // The operators, feature on the left.
    ok = expect(matches("(width >= 800px)", context), ">= inclusive") && ok;
    ok = expect(matches("(width > 799px)", context), "> exclusive") && ok;
    ok = expect(!matches("(width > 800px)", context), "> is exclusive at the bound") && ok;
    ok = expect(matches("(width <= 800px)", context), "<= inclusive") && ok;
    ok = expect(!matches("(width < 800px)", context), "< is exclusive at the bound") && ok;
    ok = expect(matches("(width = 800px)", context), "= exact") && ok;

    // Feature on the right: the operator flips.
    ok = expect(matches("(800px <= width)", context), "value on the left") && ok;
    ok = expect(!matches("(801px <= width)", context), "value on the left, false") && ok;

    // The two-sided form.
    ok = expect(matches("(600px <= width <= 900px)", context), "two-sided range") && ok;
    ok = expect(!matches("(900px <= width <= 1000px)", context), "two-sided, outside") && ok;
    ok = expect(matches("(900px >= width >= 600px)", context), "descending two-sided") && ok;

    // The same operators on height, resolution and aspect-ratio.
    ok = expect(matches("(height >= 600px)", context), "height range") && ok;
    ok = expect(matches("(resolution >= 96dpi)", context), "resolution range") && ok;
    ok = expect(matches("(aspect-ratio >= 4/3)", context), "aspect-ratio range") && ok;

    // A malformed range is not accepted.
    ok = expect(!parses("(width >= )"), "missing value is rejected") && ok;
    ok = expect(!parses("(600px <= width >= 900px)"), "mixed directions rejected") && ok;
    return ok;
}

bool test_unknown_feature_still_false() {
    MediaContext context;
    bool ok = true;
    // An unimplemented feature keeps today's behaviour: the query is false, not silently true.
    ok = expect(!matches("(color-gamut: p3)", context), "unknown feature is false") && ok;
    ok = expect(!parses("(color-gamut: p3)"), "unknown feature does not parse") && ok;
    // A prefix on a non-range feature is not valid CSS.
    ok = expect(!parses("(min-orientation: landscape)"), "min- on a keyword feature") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = test_unit_table() && ok;
    ok = test_dynamic_chrome() && ok;
    ok = test_media_viewport_units() && ok;
    ok = test_aspect_ratio() && ok;
    ok = test_hover_and_pointer() && ok;
    ok = test_range_syntax() && ok;
    ok = test_unknown_feature_still_false() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "viewport_units: all checks passed\n";
    return 0;
}
