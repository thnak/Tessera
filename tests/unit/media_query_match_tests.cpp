// SPDX-License-Identifier: MIT
//
// `useMediaQuery`'s engine half (ADR-064 item 7, issue #631): a reactive boolean for one media
// query, evaluated by the stylesheet's own evaluator.
//
// The rule the issue states is "consistent with `useBreakpoint` and the stylesheet's media
// evaluation (same engine path, no second evaluator)". These tests prove that directly: the matcher
// and the cascade are asked the same question about the same context and must agree.

#include "tessera/ui/responsive/media_query_match.hpp"
#include "tessera/ui/responsive/size_class.hpp"
#include "tessera/ui/style/media_query.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::responsive::MediaQueryMatcher;
using tessera::ui::responsive::size_class_for;
using tessera::ui::responsive::SizeClass;
using tessera::ui::style::MediaContext;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "media_query_match: " << message << "\n";
        ++failures;
    }
}

MediaContext context_of(double width) {
    MediaContext context;
    context.viewport_width = width;
    context.viewport_height = 800.0;
    return context;
}

void test_width_query() {
    MediaQueryMatcher matcher;
    expect(matcher.parse("(min-width: 600px)"), "a width query parses");
    expect(matcher.valid(), "a parsed matcher is valid");
    expect(!matcher.matches(context_of(500.0)), "500px does not match min-width 600px");
    expect(matcher.matches(context_of(600.0)), "600px matches min-width 600px");
    expect(matcher.matches(context_of(1200.0)), "1200px matches min-width 600px");
}

void test_max_width_query() {
    MediaQueryMatcher matcher;
    expect(matcher.parse("(max-width: 599px)"), "a max-width query parses");
    expect(matcher.matches(context_of(500.0)), "500px matches max-width 599px");
    expect(!matcher.matches(context_of(600.0)), "600px does not match max-width 599px");
}

// The rule that makes the hook trustworthy: the same question asked of the cascade and of the hook
// gets the same answer, because both call the same evaluator.
void test_agrees_with_the_cascade() {
    const std::string_view queries[] = {"(min-width: 600px)", "(max-width: 599px)",
                                        "(min-width: 840px)", "(orientation: landscape)"};
    const double widths[] = {320.0, 599.0, 600.0, 839.0, 840.0, 1920.0};
    for (const std::string_view query : queries) {
        MediaQueryMatcher matcher;
        if (!matcher.parse(query)) {
            expect(false, "the fixture query parses: " + std::string(query));
            continue;
        }
        for (const double width : widths) {
            const MediaContext context = context_of(width);
            const auto parsed = tessera::ui::style::parse_media_query_list(query);
            const bool cascade = tessera::ui::style::evaluate(parsed.list, context);
            expect(matcher.matches(context) == cascade, "the hook and the cascade agree on `" +
                                                            std::string(query) + "` at " +
                                                            std::to_string(width) + "px");
        }
    }
}

// `useBreakpoint` and `useMediaQuery` must not disagree about where a class starts: the class
// thresholds are the same numbers a stylesheet writes, so a query at the threshold and the class
// boundary move together.
void test_agrees_with_the_size_class() {
    MediaQueryMatcher compact;
    MediaQueryMatcher medium;
    expect(compact.parse("(max-width: 599px)"), "the compact query parses");
    expect(medium.parse("(min-width: 600px) and (max-width: 839px)"), "the medium query parses");
    for (const double width : {320.0, 599.0, 600.0, 839.0, 840.0, 1920.0}) {
        const MediaContext context = context_of(width);
        const SizeClass size_class = size_class_for(context);
        const bool is_compact = size_class == SizeClass::Compact;
        const bool is_medium = size_class == SizeClass::Medium;
        expect(compact.matches(context) == is_compact,
               "the compact query agrees with the size class at " + std::to_string(width) + "px");
        expect(medium.matches(context) == is_medium,
               "the medium query agrees with the size class at " + std::to_string(width) + "px");
    }
}

// A malformed query is `false`, not an error: a media query is a string a component may build at
// runtime, and a bad one must not take the frame down.
void test_malformed_query_is_false() {
    MediaQueryMatcher matcher;
    expect(!matcher.parse("(min-width: 900px"), "an unterminated query does not parse");
    expect(!matcher.valid(), "a malformed matcher is invalid");
    expect(!matcher.matches(context_of(1920.0)), "a malformed query never matches");
    // An empty query is not a query list either.
    expect(!matcher.parse(""), "an empty query does not parse");
    expect(!matcher.matches(context_of(1920.0)), "an empty query never matches");
}

// A colour-scheme query reads the same context field the cascade reads.
void test_color_scheme_query() {
    MediaQueryMatcher matcher;
    expect(matcher.parse("(prefers-color-scheme: dark)"), "a colour-scheme query parses");
    MediaContext light = context_of(1280.0);
    light.color_scheme = tessera::ui::style::ColorScheme::Light;
    MediaContext dark = context_of(1280.0);
    dark.color_scheme = tessera::ui::style::ColorScheme::Dark;
    expect(!matcher.matches(light), "a light context does not match prefers-color-scheme: dark");
    expect(matcher.matches(dark), "a dark context matches prefers-color-scheme: dark");
}

// A reduced-motion query reads the same context field the cascade reads.
void test_reduced_motion_query() {
    MediaQueryMatcher matcher;
    expect(matcher.parse("(prefers-reduced-motion: reduce)"), "a reduced-motion query parses");
    MediaContext normal = context_of(1280.0);
    normal.prefers_reduced_motion = false;
    MediaContext reduced = context_of(1280.0);
    reduced.prefers_reduced_motion = true;
    expect(!matcher.matches(normal), "a normal context does not match reduce");
    expect(matcher.matches(reduced), "a reduced context matches reduce");
}

// A query list is a disjunction, as in CSS.
void test_query_list_is_a_disjunction() {
    MediaQueryMatcher matcher;
    expect(matcher.parse("(max-width: 599px), (min-width: 1200px)"), "a query list parses");
    expect(matcher.matches(context_of(500.0)), "the first query matches at 500px");
    expect(!matcher.matches(context_of(800.0)), "neither query matches at 800px");
    expect(matcher.matches(context_of(1400.0)), "the second query matches at 1400px");
}

} // namespace

int main() {
    test_width_query();
    test_max_width_query();
    test_agrees_with_the_cascade();
    test_agrees_with_the_size_class();
    test_malformed_query_is_false();
    test_color_scheme_query();
    test_reduced_motion_query();
    test_query_list_is_a_disjunction();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
