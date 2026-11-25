// SPDX-License-Identifier: MIT

#include "tessera/hal/window_cutout_fold.hpp"
#include "tessera/ui/style/media_query.hpp"

#include <iostream>
#include <string_view>

namespace {

using tessera::ui::style::ColorScheme;
using tessera::ui::style::evaluate;
using tessera::ui::style::MediaContext;
using tessera::ui::style::parse_media_query_list;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "media_query: " << message << "\n";
    }
    return condition;
}

bool matches(std::string_view text, const MediaContext& context) {
    const auto parsed = parse_media_query_list(text);
    return parsed.ok && evaluate(parsed.list, context);
}

} // namespace

int main() {
    MediaContext context;
    if (!expect(matches("(min-width: 900px)", context), "min-width true") ||
        !expect(!matches("(min-width: 1400px)", context), "min-width false") ||
        !expect(matches("(max-width: 1400px)", context), "max-width true") ||
        !expect(matches("(min-height: 700px)", context), "min-height") ||
        !expect(matches("(orientation: landscape)", context), "landscape") ||
        !expect(!matches("(orientation: portrait)", context), "landscape not portrait") ||
        !expect(matches("screen and (orientation: landscape)", context), "screen and") ||
        !expect(!matches("print and (min-width: 1px)", context), "print false") ||
        !expect(matches("all", context), "all true")) {
        return 1;
    }

    if (!expect(matches("(prefers-color-scheme: light)", context), "color light") ||
        !expect(!matches("(prefers-color-scheme: dark)", context), "color dark false") ||
        !expect(matches("(prefers-reduced-motion: no-preference)", context), "motion default") ||
        !expect(!matches("(prefers-reduced-motion: reduce)", context), "motion reduce false")) {
        return 2;
    }

    context.color_scheme = ColorScheme::Dark;
    context.prefers_reduced_motion = true;
    if (!expect(matches("(prefers-color-scheme: dark)", context), "dark context") ||
        !expect(matches("(prefers-reduced-motion: reduce)", context), "reduce context")) {
        return 3;
    }

    context.resolution_dpi = 192.0;
    if (!expect(matches("(min-resolution: 2dppx)", context), "resolution dppx") ||
        !expect(matches("(resolution: 192dpi)", context), "resolution dpi") ||
        !expect(!matches("(min-resolution: 3dppx)", context), "resolution false")) {
        return 4;
    }

    if (!expect(matches("(max-width: 600px), (min-width: 1000px)", context), "comma or") ||
        !expect(matches("not (min-width: 2000px)", context), "not query") ||
        !expect(!matches("not (min-width: 900px)", context), "not query false")) {
        return 5;
    }

    context.viewport_width = 600.0;
    context.viewport_height = 900.0;
    if (!expect(matches("(max-width: 600px)", context), "exact width") ||
        !expect(matches("(orientation: portrait)", context), "portrait") ||
        !expect(matches("(max-width: 40rem)", context), "rem width")) {
        return 6;
    }

    if (!expect(!parse_media_query_list("").ok, "empty invalid") ||
        !expect(!parse_media_query_list("(min-width: 900px").ok, "unterminated invalid") ||
        !expect(!matches("(min-width: bad)", context), "invalid length false") ||
        !expect(!matches("(unknown-feature: 1px)", context), "unknown feature false")) {
        return 7;
    }

    // `@media (window-backdrop: ...)` matches the platform's resolved material, never a request
    // (glass-v1.md section 6). The default context is the null/headless answer: no material, so a
    // stylesheet's opaque fallback applies.
    {
        MediaContext backdrop_context;
        using tessera::hal::BackdropMaterial;
        if (!expect(matches("(window-backdrop: none)", backdrop_context),
                    "backdrop none default") ||
            !expect(!matches("(window-backdrop: mica)", backdrop_context),
                    "backdrop mica false by default")) {
            return 8;
        }
        backdrop_context.window_backdrop = BackdropMaterial::Mica;
        if (!expect(matches("(window-backdrop: mica)", backdrop_context), "backdrop mica") ||
            !expect(!matches("(window-backdrop: none)", backdrop_context),
                    "backdrop none false on mica") ||
            !expect(matches("NOT (window-backdrop: none)", backdrop_context), "backdrop negated")) {
            return 9;
        }
        backdrop_context.window_backdrop = BackdropMaterial::Acrylic;
        if (!expect(matches("(window-backdrop: acrylic)", backdrop_context), "backdrop acrylic") ||
            !expect(!matches("(window-backdrop: blur)", backdrop_context), "acrylic is not blur")) {
            return 10;
        }
        backdrop_context.window_backdrop = BackdropMaterial::Blur;
        if (!expect(matches("(window-backdrop: BLUR)", backdrop_context),
                    "backdrop keyword is case-insensitive") ||
            // `auto` is a request, never a resolved material: it cannot match.
            !expect(!matches("(window-backdrop: auto)", backdrop_context),
                    "backdrop auto never matches") ||
            !expect(!matches("(window-backdrop: fancy)", backdrop_context),
                    "unknown backdrop value does not match")) {
            return 11;
        }
    }

    // Fold features (ADR-074): the standard `device-posture` and viewport-segment names, answered
    // from the fold the window reports and from nothing else.
    {
        using tessera::hal::FoldInfo;
        using tessera::hal::FoldOrientation;
        using tessera::hal::FoldState;
        const MediaContext no_fold;
        if (!expect(matches("(device-posture: continuous)", no_fold), "no fold is continuous") ||
            !expect(!matches("(device-posture: folded)", no_fold), "no fold is not folded") ||
            !expect(matches("(horizontal-viewport-segments: 1)", no_fold) &&
                        matches("(vertical-viewport-segments: 1)", no_fold),
                    "no fold is one segment on each axis") ||
            !expect(matches("(horizontal-viewport-segments < 2)", no_fold),
                    "the range form compares the count")) {
            return 12;
        }

        FoldInfo book;
        book.state = FoldState::HalfOpened;
        book.orientation = FoldOrientation::Vertical;
        book.separating = true;
        const MediaContext book_context = tessera::ui::style::with_fold(MediaContext{}, book);
        if (!expect(matches("(device-posture: folded)", book_context),
                    "a half-open fold is folded") ||
            !expect(!matches("(device-posture: continuous)", book_context),
                    "folded is not continuous") ||
            !expect(matches("(horizontal-viewport-segments: 2)", book_context) &&
                        matches("(horizontal-viewport-segments >= 2)", book_context) &&
                        matches("(min-horizontal-viewport-segments: 2)", book_context) &&
                        matches("(1 < horizontal-viewport-segments <= 2)", book_context),
                    "a vertical hinge is two horizontal segments, in every range form") ||
            !expect(matches("(vertical-viewport-segments: 1)", book_context),
                    "and one vertical segment") ||
            !expect(matches("(device-posture: folded) and (horizontal-viewport-segments: 2)",
                            book_context),
                    "the two features combine")) {
            return 13;
        }

        FoldInfo tabletop = book;
        tabletop.orientation = FoldOrientation::Horizontal;
        const MediaContext tabletop_context =
            tessera::ui::style::with_fold(MediaContext{}, tabletop);
        if (!expect(matches("(vertical-viewport-segments: 2)", tabletop_context) &&
                        matches("(horizontal-viewport-segments: 1)", tabletop_context),
                    "a horizontal hinge is two vertical segments")) {
            return 14;
        }

        // A fold that does not separate is one segment: a flexible screen with no divider.
        FoldInfo bendable = book;
        bendable.separating = false;
        const MediaContext bendable_context =
            tessera::ui::style::with_fold(MediaContext{}, bendable);
        FoldInfo flat;
        flat.state = FoldState::Flat;
        const MediaContext flat_context = tessera::ui::style::with_fold(book_context, flat);
        FoldInfo no_axis = book;
        no_axis.orientation = FoldOrientation::None;
        const MediaContext no_axis_context = tessera::ui::style::with_fold(MediaContext{}, no_axis);
        if (!expect(matches("(device-posture: folded)", bendable_context) &&
                        matches("(horizontal-viewport-segments: 1)", bendable_context),
                    "a half-open fold that does not separate is folded but one segment") ||
            !expect(matches("(device-posture: continuous)", flat_context) &&
                        matches("(horizontal-viewport-segments: 1)", flat_context),
                    "a flat fold is continuous, and unfolding resets the segments") ||
            !expect(matches("(horizontal-viewport-segments: 1)", no_axis_context) &&
                        matches("(vertical-viewport-segments: 1)", no_axis_context),
                    "a separating fold with no axis has nothing to split")) {
            return 15;
        }

        // The hinge angle and rectangle are not media answers: a fold that differs only there
        // leaves the context field-for-field equal, so a slow fold re-resolves nothing.
        FoldInfo rotating = book;
        rotating.hinge_angle_degrees = 95.0f;
        rotating.hinge_bounds = {100.0f, 0.0f, 24.0f, 800.0f};
        FoldInfo rotated = rotating;
        rotated.hinge_angle_degrees = 96.0f;
        rotated.hinge_bounds = {140.0f, 0.0f, 24.0f, 800.0f};
        const MediaContext before = tessera::ui::style::with_fold(MediaContext{}, rotating);
        const MediaContext after = tessera::ui::style::with_fold(MediaContext{}, rotated);
        if (!expect(before.device_posture == after.device_posture &&
                        before.horizontal_viewport_segments == after.horizontal_viewport_segments &&
                        before.vertical_viewport_segments == after.vertical_viewport_segments &&
                        before.viewport_width == after.viewport_width,
                    "a changed hinge angle or rectangle leaves the media context equal")) {
            return 16;
        }

        // Grammar: the posture is a keyword feature with no min/max form, and the segment count
        // is an integer, so neither accepts what the other takes.
        if (!expect(!parse_media_query_list("(min-device-posture: folded)").ok,
                    "device-posture has no min- prefix") ||
            !expect(!matches("(device-posture: tabletop)", book_context),
                    "an unknown posture keyword never matches") ||
            !expect(!matches("(device-posture >= folded)", book_context),
                    "device-posture has no range form") ||
            !expect(!matches("(horizontal-viewport-segments: 2px)", book_context),
                    "a segment count with a unit never matches") ||
            !expect(!matches("(horizontal-viewport-segments: 1.5)", book_context),
                    "a segment count that is not an integer never matches") ||
            !expect(!matches("(horizontal-viewport-segments: two)", book_context),
                    "a segment count that is not a number never matches")) {
            return 17;
        }
    }

    std::cout << "media_query: ok\n";
    return 0;
}
