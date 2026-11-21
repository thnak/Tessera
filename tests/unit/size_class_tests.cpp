// SPDX-License-Identifier: MIT
//
// The window size classes of docs/design/responsive.md section 3 and
// docs/specs/jsx-subset-v1.md section 7.1 (issue #692): the boundaries the hook, the preview's
// `viewport.set` report and an app's own `@media` rules must all agree on.

#include "tessera/ui/responsive/size_class.hpp"

#include <iostream>
#include <string_view>

namespace {

using tessera::ui::responsive::size_class_for;
using tessera::ui::responsive::size_class_name;
using tessera::ui::responsive::SizeClass;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "size_class: " << message << "\n";
    }
    return condition;
}

int boundaries() {
    // The four widths the issue names, plus the two just below each boundary: a threshold that is
    // off by one is the classic breakpoint bug, so both sides are asserted.
    if (!expect(size_class_for(599.0) == SizeClass::Compact, "599 is compact") ||
        !expect(size_class_for(600.0) == SizeClass::Medium, "600 is medium") ||
        !expect(size_class_for(839.0) == SizeClass::Medium, "839 is medium") ||
        !expect(size_class_for(840.0) == SizeClass::Expanded, "840 is expanded")) {
        return 1;
    }
    if (!expect(size_class_for(0.0) == SizeClass::Compact, "0 is compact") ||
        !expect(size_class_for(320.0) == SizeClass::Compact, "320 is compact") ||
        !expect(size_class_for(1920.0) == SizeClass::Expanded, "1920 is expanded")) {
        return 2;
    }
    return 0;
}

int degenerate_widths() {
    // A window that has not been measured yet is the smallest class, so a component that branches
    // on the class renders its narrow layout rather than its widest one.
    if (!expect(size_class_for(-1.0) == SizeClass::Compact, "a negative width is compact") ||
        !expect(size_class_for(std::numeric_limits<double>::quiet_NaN()) == SizeClass::Compact,
                "NaN is compact") ||
        !expect(size_class_for(std::numeric_limits<double>::infinity()) == SizeClass::Compact,
                "infinity is compact")) {
        return 3;
    }
    return 0;
}

int media_context_agrees() {
    // The hook and the `@media` evaluator read the same field, so a stylesheet breakpoint and the
    // hook cannot disagree about where a class starts.
    tessera::ui::style::MediaContext context;
    context.viewport_width = 700.0;
    if (!expect(size_class_for(context) == SizeClass::Medium, "the media context width is used")) {
        return 4;
    }
    context.viewport_width = 500.0;
    if (!expect(size_class_for(context) == SizeClass::Compact, "a narrower context is compact")) {
        return 5;
    }
    return 0;
}

int names_round_trip() {
    for (const SizeClass value : {SizeClass::Compact, SizeClass::Medium, SizeClass::Expanded}) {
        const std::string_view name = size_class_name(value);
        const std::optional<SizeClass> parsed = tessera::ui::responsive::parse_size_class(name);
        if (!expect(parsed.has_value() && *parsed == value,
                    "the TSX spelling round-trips: " + std::string(name))) {
            return 6;
        }
    }
    if (!expect(!tessera::ui::responsive::parse_size_class("wide").has_value(),
                "an unknown spelling is rejected")) {
        return 7;
    }
    return 0;
}

} // namespace

int main() {
    if (const int rc = boundaries(); rc != 0) {
        return rc;
    }
    if (const int rc = degenerate_widths(); rc != 0) {
        return rc;
    }
    if (const int rc = media_context_agrees(); rc != 0) {
        return rc;
    }
    if (const int rc = names_round_trip(); rc != 0) {
        return rc;
    }
    std::cout << "size_class: ok\n";
    return 0;
}
