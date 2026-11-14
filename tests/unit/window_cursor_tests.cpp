// SPDX-License-Identifier: MIT
//
// The CSS `cursor` value grammar (issue #191, docs/specs/window-chrome-v1.md section 8.5): every
// supported keyword parses to its kind, the keyword match is case- and whitespace-insensitive, an
// unknown keyword falls back to `auto`, a `url()` cursor list is unsupported, and the declaration
// validator reports exactly one `UnsupportedValue` for an unsupported form while keeping the
// declaration. Display-free: no window, no HAL, no platform.

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/cursor_value.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/supports.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::cursor_kind_name;
using tessera::ui::style::CursorKind;
using tessera::ui::style::evaluate_supports;
using tessera::ui::style::is_cursor_keyword;
using tessera::ui::style::is_supported_cursor_value;
using tessera::ui::style::parse_cursor_kind;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleDiagnosticKind;
using tessera::ui::style::StyleNode;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_cursor: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Every supported keyword with its expected kind. Written out rather than derived from
// `cursor_kind_name`, so a name/kind swap in the table is caught instead of round-tripped.
struct KeywordCase {
    std::string_view keyword;
    CursorKind kind;
};

constexpr std::array<KeywordCase, 36> kKeywordCases{{
    {"auto", CursorKind::Auto},
    {"default", CursorKind::Default},
    {"none", CursorKind::None},
    {"context-menu", CursorKind::ContextMenu},
    {"help", CursorKind::Help},
    {"pointer", CursorKind::Pointer},
    {"progress", CursorKind::Progress},
    {"wait", CursorKind::Wait},
    {"cell", CursorKind::Cell},
    {"crosshair", CursorKind::Crosshair},
    {"text", CursorKind::Text},
    {"vertical-text", CursorKind::VerticalText},
    {"alias", CursorKind::Alias},
    {"copy", CursorKind::Copy},
    {"move", CursorKind::Move},
    {"no-drop", CursorKind::NoDrop},
    {"not-allowed", CursorKind::NotAllowed},
    {"grab", CursorKind::Grab},
    {"grabbing", CursorKind::Grabbing},
    {"all-scroll", CursorKind::AllScroll},
    {"zoom-in", CursorKind::ZoomIn},
    {"zoom-out", CursorKind::ZoomOut},
    {"n-resize", CursorKind::ResizeN},
    {"e-resize", CursorKind::ResizeE},
    {"s-resize", CursorKind::ResizeS},
    {"w-resize", CursorKind::ResizeW},
    {"ne-resize", CursorKind::ResizeNe},
    {"nw-resize", CursorKind::ResizeNw},
    {"se-resize", CursorKind::ResizeSe},
    {"sw-resize", CursorKind::ResizeSw},
    {"ns-resize", CursorKind::ResizeNs},
    {"ew-resize", CursorKind::ResizeEw},
    {"nesw-resize", CursorKind::ResizeNesw},
    {"nwse-resize", CursorKind::ResizeNwse},
    {"col-resize", CursorKind::ColResize},
    {"row-resize", CursorKind::RowResize},
}};

bool matches(std::string_view text) {
    static const PropertyRegistry registry;
    const auto parsed = parse_supports_condition(text);
    return parsed.ok && evaluate_supports(parsed.condition, registry);
}

// Every supported keyword parses to its kind, and the case/whitespace-insensitive forms agree.
int test_grammar() {
    for (const KeywordCase& entry : kKeywordCases) {
        if (!expect(parse_cursor_kind(entry.keyword) == entry.kind,
                    "a supported keyword parses to its kind") ||
            !expect(is_supported_cursor_value(entry.keyword),
                    "every parsed keyword is reported supported") ||
            !expect(is_cursor_keyword(entry.keyword),
                    "every supported keyword is a single identifier") ||
            !expect(cursor_kind_name(entry.kind) == entry.keyword,
                    "the kind name round-trips to the CSS keyword")) {
            std::cerr << "  keyword: " << entry.keyword << "\n";
            return 1;
        }
    }

    // Case and surrounding whitespace are insignificant, as everywhere else in the value grammars.
    if (!expect(parse_cursor_kind("  POINTER\t") == CursorKind::Pointer,
                "the keyword match is trimmed and case-insensitive") ||
        !expect(parse_cursor_kind("Not-Allowed\n") == CursorKind::NotAllowed,
                "a hyphenated keyword is case-insensitive") ||
        !expect(is_supported_cursor_value("  TeXt ") == true,
                "support is trimmed and lowercased")) {
        return 2;
    }

    // An unknown keyword, an empty value and a `url()` list all fall back to the initial `auto`:
    // the CSS fallback, so a value V1 did not implement never becomes a shape the platform cannot
    // map.
    if (!expect(parse_cursor_kind("nonsense") == CursorKind::Auto,
                "an unknown keyword falls back to auto") ||
        !expect(parse_cursor_kind("") == CursorKind::Auto, "an empty value falls back to auto") ||
        !expect(parse_cursor_kind("url(a.png)") == CursorKind::Auto,
                "a url() cursor falls back to auto") ||
        !expect(parse_cursor_kind("url(a.png), pointer") == CursorKind::Auto,
                "a url() cursor list falls back to auto") ||
        !expect(!is_supported_cursor_value("url(a.png)"), "a url() cursor is unsupported") ||
        !expect(!is_supported_cursor_value("url(a.png), pointer"),
                "a url() cursor list is unsupported") ||
        !expect(!is_supported_cursor_value("pointer, text"), "a comma list is unsupported") ||
        !expect(!is_cursor_keyword("url(a.png)"), "a url() cursor is not a bare keyword") ||
        !expect(!is_cursor_keyword("pointer, text"), "a comma list is not a bare keyword")) {
        return 3;
    }

    // `initial`/`inherit` are the cascade's business: here they are simply not supported values,
    // and the sync resolves them to `auto` (the cascade has already substituted them by then).
    if (!expect(!is_supported_cursor_value("inherit"),
                "a CSS-wide keyword is not a cursor keyword") ||
        !expect(parse_cursor_kind("inherit") == CursorKind::Auto,
                "a CSS-wide keyword falls back to auto at this layer")) {
        return 4;
    }
    return 0;
}

// The declaration validator reports exactly one UnsupportedValue for a `url()` cursor and keeps the
// declaration, so the computed value is still readable (it falls back to `auto`).
int test_diagnostics() {
    const auto unsupported = compile_stylesheet(".a { cursor: url(a.png); }");
    if (!expect(unsupported.diagnostics().size() == 1,
                "a url() cursor reports exactly one diagnostic") ||
        !expect(unsupported.diagnostics()[0].kind == StyleDiagnosticKind::UnsupportedValue,
                "the diagnostic kind is UnsupportedValue") ||
        !expect(unsupported.diagnostics()[0].property == "cursor",
                "the diagnostic names the property") ||
        !expect(unsupported.rules().size() == 1, "the declaration stays in the AST")) {
        return 5;
    }

    const auto list = compile_stylesheet(".b { cursor: url(a.png), pointer; }");
    if (!expect(list.diagnostics().size() == 1,
                "a url() cursor list reports exactly one diagnostic") ||
        !expect(list.rules().size() == 1, "the url() list declaration stays in the AST")) {
        return 6;
    }

    const auto unknown = compile_stylesheet(".c { cursor: nonsense; }");
    if (!expect(unknown.diagnostics().size() == 1,
                "an unknown keyword reports exactly one diagnostic") ||
        !expect(unknown.rules().size() == 1, "the unknown-keyword declaration stays in the AST")) {
        return 7;
    }

    // Every supported keyword is accepted silently, and the computed value is the keyword.
    const auto supported = compile_stylesheet(".d { cursor: pointer; }"
                                              ".e { cursor: Not-Allowed; }");
    if (!expect(supported.diagnostics().empty(), "supported keywords report nothing")) {
        return 8;
    }
    StyleNode view;
    view.tag = "div";
    view.classes = {"d"};
    const ComputedStyle style = supported.compute(view, nullptr);
    const std::string* value = style.find("cursor");
    if (!expect(value != nullptr && *value == "pointer", "the computed cursor is the keyword")) {
        return 9;
    }

    // A `var()` reference is judged after substitution, so it is not a value-form diagnostic here:
    // the declaration is kept and the unresolved reference reports its own kind at computed-value
    // time instead of being misreported as an unsupported cursor form.
    const auto unresolved = compile_stylesheet(".f { cursor: var(--shape); }");
    if (!expect(unresolved.rules().size() == 1, "the var() cursor declaration is kept")) {
        return 10;
    }
    for (const auto& diagnostic : unresolved.diagnostics()) {
        if (!expect(diagnostic.kind != StyleDiagnosticKind::UnsupportedValue,
                    "a var() cursor is not an UnsupportedValue")) {
            return 11;
        }
    }
    return 0;
}

// `@supports` is exactly as strict as the validator: the same keyword table answers both.
int test_supports() {
    if (!expect(matches("(cursor: pointer)"), "@supports cursor pointer") ||
        !expect(matches("(cursor:  POINTER )"), "@supports is case/space-insensitive") ||
        !expect(matches("(cursor: nwse-resize)"), "@supports a resize keyword") ||
        !expect(!matches("(cursor: url(a.png))"), "@supports rejects a url() cursor") ||
        !expect(!matches("(cursor: nonsense)"), "@supports rejects an unknown keyword")) {
        return 11;
    }
    return 0;
}

} // namespace

int main() {
    for (int (*test)() : {&test_grammar, &test_diagnostics, &test_supports}) {
        const int code = test();
        if (code != 0) {
            return code;
        }
    }
    std::cout << "window_cursor: ok\n";
    return 0;
}
