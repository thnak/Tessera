// SPDX-License-Identifier: MIT
//
// The schema inline-style table and the runtime property registry must agree (issue #272).
//
// `src/schema/src/style.cpp` is what the JSX front end validates `style={{ ... }}` keys against and
// what decides whether a number becomes "<n>px" (docs/design/jsx-ir-lowering.md decision 10).
// `src/ui/style/property_registry.{cpp,extra.cpp}` plus `logical_properties.cpp` are the engine's
// list of longhands. They drifted apart once: the schema table was frozen at 73 rows while the
// registry grew to 119, so a valid `style={{ pointerEvents: "none" }}` in a project failed lowering
// with `TESSERA_IR_UNKNOWN_PROP`.
//
// The schema library must not link or include the UI module (machine-readable.md section 4,
// `schema_layering`), so the table cannot be generated from the registry at build time. This test
// is the seam instead: the only place both are visible at once. It walks both directions and checks
// each kind against the registry's grammar:
//
//   * every registered longhand is a row, with the kind its grammar implies;
//   * every row is a registered longhand, not a shorthand (a shorthand must be expanded by the
//     registry, never stored as one computed value);
//   * a row's kind agrees with the grammar the registry enforces: a `Length` row accepts a px
//     length, a `Number` row accepts a bare number, a `Color` row accepts a colour, and every row
//     accepts its own registered initial value with no diagnostic.
//
// A failure names the property and the table to change, so the repair is mechanical.

#include "tessera/schema/style.hpp"
#include "tessera/ui/style/property_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::schema::style_properties;
using tessera::schema::StyleKind;
using tessera::schema::StyleProperty;
using tessera::ui::style::Declaration;
using tessera::ui::style::PropertyExpansionResult;
using tessera::ui::style::PropertyInfo;
using tessera::ui::style::PropertyRegistry;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "schema_style_registry: " << message << "\n";
    }
    return condition;
}

const char* kind_name(StyleKind kind) {
    switch (kind) {
    case StyleKind::Length:
        return "length";
    case StyleKind::Number:
        return "number";
    case StyleKind::Color:
        return "color";
    case StyleKind::Keyword:
        return "keyword";
    case StyleKind::List:
        return "list";
    }
    return "?";
}

// The registry accepts `value` for `property` (one stored declaration, no diagnostic). This is the
// one probe that reads the registry's real grammar rather than just its name set.
bool accepts(const PropertyRegistry& registry, std::string_view property, std::string_view value) {
    const PropertyExpansionResult result =
        registry.expand(Declaration{std::string(property), std::string(value), false, 0});
    return result.diagnostics.empty() && !result.declarations.empty();
}

// A sample value the row's kind must accept. `Length` and `Number` are the two kinds decision 10
// distinguishes (px versus unitless), so their samples are the ones a wrong kind would break.
const char* sample_for(StyleKind kind) {
    switch (kind) {
    case StyleKind::Length:
        return "2px";
    case StyleKind::Number:
        return "0.5";
    case StyleKind::Color:
        return "rebeccapurple";
    case StyleKind::Keyword:
    case StyleKind::List:
        return nullptr;
    }
    return nullptr;
}

// Rows whose kind is not obvious from the name, pinned so a future edit is deliberate rather than a
// quiet change to what a JSX number means.
struct PinnedKind {
    const char* name;
    StyleKind kind;
    const char* why;
};

constexpr PinnedKind kPinnedKinds[] = {
    {"aspect-ratio", StyleKind::Number,
     "a bare number is the unitless ratio; `2px` is not a ratio"},
    {"line-height", StyleKind::Number, "a bare number is the unitless multiplier"},
    {"font-weight", StyleKind::Number, "a number stays unitless"},
    {"z-index", StyleKind::Number, "an integer, never a length"},
    {"flex-grow", StyleKind::Number, "a unitless flex factor"},
    {"flex-shrink", StyleKind::Number, "a unitless flex factor"},
    {"opacity", StyleKind::Number, "a unitless 0..1 factor"},
    {"order", StyleKind::Number, "an integer, never a length"},
    {"animation-duration", StyleKind::Number,
     "a bare number is unitless seconds; `2px` is not a time"},
    {"animation-delay", StyleKind::Number, "a bare number is unitless seconds"},
    {"animation-iteration-count", StyleKind::Number, "a unitless count"},
    {"transition-duration", StyleKind::Number, "a bare number is unitless seconds"},
    {"transition-delay", StyleKind::Number, "a bare number is unitless seconds"},
    {"transform", StyleKind::List,
     "a function list; a number is not a transform and must not become `2px`"},
    {"box-shadow", StyleKind::List, "a comma-separated layer list"},
    {"grid-template-columns", StyleKind::List,
     "a track list; `<n>px` is not a track, so a number must not become one"},
    {"grid-template-rows", StyleKind::List, "a track list"},
    {"grid-column", StyleKind::Keyword, "a `<start> / <end>` line, not a length"},
    {"grid-row", StyleKind::Keyword, "a `<start> / <end>` line, not a length"},
    {"transform-origin", StyleKind::Length, "one or two lengths (`50% 50%`)"},
    {"font-family", StyleKind::List, "a comma-separated family list"},
    {"transition-property", StyleKind::List, "a comma-separated property name list"},
    {"-webkit-backdrop-filter", StyleKind::Keyword, "an alias of `backdrop-filter`, same grammar"},
};

// One row of the schema table, keyed by name for the registry walk.
struct Row {
    std::string_view name;
    StyleKind kind;
};

bool check_rows_are_registered_longhands(const PropertyRegistry& registry,
                                         const std::vector<Row>& rows) {
    bool ok = true;
    for (const Row& row : rows) {
        const PropertyInfo* info = registry.find(row.name);
        if (info == nullptr) {
            ok = expect(false, "`" + std::string(row.name) +
                                   "` is a schema style row but the registry does not register it; "
                                   "remove the row from src/schema/src/style.cpp or fix its "
                                   "spelling to the registry's") &&
                 ok;
            continue;
        }
        if (registry.longhands(row.name) != nullptr) {
            ok = expect(false, "`" + std::string(row.name) +
                                   "` is a schema style row but the registry expands it as a "
                                   "shorthand; rows are longhands only") &&
                 ok;
            continue;
        }
        // The row's own initial value is the registry's evidence of the grammar it enforces, so a
        // row whose kind disagrees with it is a row that would translate JSX numbers wrongly.
        if (!info->initial.empty() && !accepts(registry, row.name, info->initial)) {
            ok = expect(false, "`" + std::string(row.name) + "` is a " +
                                   std::string(kind_name(row.kind)) +
                                   " row but the registry rejects its own initial value `" +
                                   info->initial + "`") &&
                 ok;
        }
        if (const char* sample = sample_for(row.kind);
            sample != nullptr && !accepts(registry, row.name, sample)) {
            ok = expect(false, "`" + std::string(row.name) + "` is a " +
                                   std::string(kind_name(row.kind)) +
                                   " row but the registry rejects `" + sample +
                                   "`; fix the row's kind in src/schema/src/style.cpp") &&
                 ok;
        }
    }
    return ok;
}

// The direction that issue #272 broke: a property the engine accepts in a stylesheet but the JSX
// front end rejects in an inline `style` object.
bool check_every_longhand_has_a_row(const PropertyRegistry& registry,
                                    const std::vector<Row>& rows) {
    bool ok = true;
    for (const std::string_view name : registry.longhand_names()) {
        const auto found = std::find_if(rows.begin(), rows.end(),
                                        [&](const Row& row) { return row.name == name; });
        if (found == rows.end()) {
            ok = expect(false, "`" + std::string(name) +
                                   "` is a registered longhand but has no schema style row, so "
                                   "JSX `style` rejects it with TESSERA_IR_UNKNOWN_PROP; add it "
                                   "to src/schema/src/style.cpp with the kind its grammar takes") &&
                 ok;
        }
    }
    return ok;
}

bool check_pinned_kinds(const std::vector<Row>& rows) {
    bool ok = true;
    for (const PinnedKind& pinned : kPinnedKinds) {
        const auto found = std::find_if(rows.begin(), rows.end(),
                                        [&](const Row& row) { return row.name == pinned.name; });
        if (found == rows.end()) {
            ok = expect(false, "`" + std::string(pinned.name) +
                                   "` has no schema style row any more; delete its kPinnedKinds "
                                   "entry") &&
                 ok;
            continue;
        }
        if (found->kind != pinned.kind) {
            ok = expect(false, "`" + std::string(pinned.name) + "` is listed as " +
                                   kind_name(found->kind) + " but must be " +
                                   kind_name(pinned.kind) + ": " + pinned.why) &&
                 ok;
        }
    }
    return ok;
}

} // namespace

int main() {
    const PropertyRegistry registry;

    std::vector<Row> rows;
    rows.reserve(style_properties().size());
    for (const StyleProperty& property : style_properties()) {
        rows.push_back(Row{property.name, property.kind});
    }
    if (!expect(!rows.empty(), "the schema style table lists no property")) {
        return 1;
    }

    bool ok = true;
    ok = check_rows_are_registered_longhands(registry, rows) && ok;
    ok = check_every_longhand_has_a_row(registry, rows) && ok;
    ok = check_pinned_kinds(rows) && ok;
    ok = expect(rows.size() == registry.longhand_names().size(),
                "the schema style table and the registry list a different number of properties (" +
                    std::to_string(rows.size()) + " rows, " +
                    std::to_string(registry.longhand_names().size()) + " longhands)") &&
         ok;

    if (!ok) {
        return 1;
    }
    std::cout << "schema_style_registry: " << rows.size()
              << " schema style rows agree with the registry\n";
    return 0;
}
