// SPDX-License-Identifier: MIT
//
// The V1 compatibility matrix and PropertyRegistry must agree (issue #250).
//
// `docs/specs/stylesheet-v1.md` section 11 is the prose promise of what V1 supports; the property
// names behind it live in `docs/specs/stylesheet-v1.properties.txt` so this test can read them
// without parsing a markdown table (the spec is at its 600-line hard limit, its cells carry prose
// and escapes, and a separate file keeps the list reviewable in a diff). This test walks both
// directions:
//
//   * every property the matrix promises is registered, so the engine cannot silently drop one;
//   * every property the registry registers is in the matrix, so the spec cannot fall behind.
//
// A failure names the property and its section 11 row, never a bare count: the reader has to land
// on the paragraph that promises the property. Two allow-lists keep the check honest:
//
//   * `kKnownGaps` are V1 properties the engine does not implement yet. Each entry names the issue
//     that closes it, and the entry must be deleted when it does: a gap that is now satisfied is
//     itself a failure, so the list can only shrink.
//   * `kEngineInternal` are registered properties section 11 has no row for. They are real engine
//     behaviour documented elsewhere, not spec promises, so they are listed with the section that
//     does document them.

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/supports.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_STYLESHEET_SPEC_DIR
#error "TESSERA_STYLESHEET_SPEC_DIR must be defined by the build"
#endif

namespace {

using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::evaluate_supports;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PropertyExpansionResult;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleNode;

// One line of the machine-readable matrix: `<kind> <name> <section 11 row>`.
struct MatrixEntry {
    std::string kind;
    std::string name;
    std::string row;
};

// A property the check must not report, with the reason it is exempt.
struct AllowEntry {
    const char* name;
    const char* reason;
};

// V1 properties the engine does not implement yet. Delete an entry when its issue lands; the
// stale-entry check below fails until it is gone.
// Empty: the Grid subset landed with issue #249, so no V1 property is excused any more. The list
// (and the stale-entry check that reads it) stays so the next gap has a recorded place.
constexpr std::array<AllowEntry, 0> kKnownGaps{};

// Registered properties section 11 has no row for. These are engine behaviour, not spec promises.
constexpr AllowEntry kEngineInternal[] = {
    {"visibility",
     "inherited and read by paint (section 7.3 lists it), but section 11 has no row for it"},
    {"writing-mode",
     "inherited and read by layout (section 7.3 lists it), but section 11 has no row for it"},
};

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "property_registry_matrix: " << message << "\n";
    }
    return condition;
}

const char* allow_reason(const AllowEntry* entries, std::size_t count, std::string_view name) {
    for (std::size_t i = 0; i < count; ++i) {
        if (name == entries[i].name) {
            return entries[i].reason;
        }
    }
    return nullptr;
}

bool is_known_gap(std::string_view name) {
    return allow_reason(kKnownGaps.data(), kKnownGaps.size(), name) != nullptr;
}

// Split on runs of spaces and tabs; the file is whitespace separated by design.
std::vector<std::string_view> split_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = std::string_view::npos;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            if (start != std::string_view::npos) {
                fields.push_back(line.substr(start, i - start));
                start = std::string_view::npos;
            }
            continue;
        }
        if (start == std::string_view::npos) {
            start = i;
        }
    }
    if (start != std::string_view::npos) {
        fields.push_back(line.substr(start));
    }
    return fields;
}

bool load_matrix(std::vector<MatrixEntry>& entries) {
    const std::string path =
        std::string(TESSERA_STYLESHEET_SPEC_DIR) + "/stylesheet-v1.properties.txt";
    std::ifstream file(path);
    if (!file) {
        std::cerr << "property_registry_matrix: cannot open " << path << "\n";
        return false;
    }

    std::string line;
    std::size_t number = 0;
    bool ok = true;
    while (std::getline(file, line)) {
        ++number;
        const std::vector<std::string_view> fields = split_fields(line);
        if (fields.empty() || fields.front().front() == '#') {
            continue;
        }
        if (fields.size() != 3) {
            std::cerr << "property_registry_matrix: " << path << ":" << number
                      << " expected `<kind> <name> <row>`, found " << fields.size()
                      << " field(s)\n";
            ok = false;
            continue;
        }
        const std::string_view kind = fields[0];
        if (kind != "longhand" && kind != "shorthand" && kind != "external") {
            std::cerr << "property_registry_matrix: " << path << ":" << number << " unknown kind `"
                      << kind << "` (expected longhand, shorthand or external)\n";
            ok = false;
            continue;
        }
        entries.push_back(
            MatrixEntry{std::string(kind), std::string(fields[1]), std::string(fields[2])});
    }
    if (entries.empty()) {
        std::cerr << "property_registry_matrix: " << path << " lists no property\n";
        return false;
    }
    return ok;
}

// The matrix promises `name` in `row`; the registry must provide it.
bool check_promised(const PropertyRegistry& registry, const MatrixEntry& entry) {
    const std::string where = "`" + entry.name + "` (section 11 row `" + entry.row + "`)";
    if (entry.kind == "shorthand") {
        const std::vector<std::string>* longhands = registry.longhands(entry.name);
        if (longhands == nullptr) {
            if (is_known_gap(entry.name)) {
                return true;
            }
            return expect(false, where + " is promised as a shorthand but the registry does not "
                                         "expand it");
        }
        if (longhands->empty()) {
            return expect(false, where + " expands to no longhand");
        }
        for (const std::string& longhand : *longhands) {
            if (registry.find(longhand) == nullptr) {
                return expect(false, where + " expands to `" + longhand +
                                         "`, which the registry does not register");
            }
        }
        return true;
    }

    if (registry.find(entry.name) == nullptr) {
        if (is_known_gap(entry.name)) {
            return true;
        }
        return expect(false, where + " is promised as a " + entry.kind +
                                 " but the registry does not register it");
    }
    if (entry.kind == "external" && registry.longhands(entry.name) != nullptr) {
        return expect(false, where + " is listed as external but the registry expands it as a "
                                     "shorthand; list it as a shorthand instead");
    }
    return true;
}

// Every registered name must be in the matrix, so the spec cannot fall behind the engine.
bool check_registered(const PropertyRegistry& registry, const std::vector<MatrixEntry>& entries) {
    bool ok = true;
    for (const std::string_view name : registry.longhand_names()) {
        bool listed = false;
        for (const MatrixEntry& entry : entries) {
            if (entry.name == name && entry.kind != "shorthand") {
                listed = true;
                break;
            }
        }
        if (listed) {
            continue;
        }
        if (const char* reason = allow_reason(kEngineInternal, std::size(kEngineInternal), name)) {
            (void)reason;
            continue;
        }
        ok = expect(false, "`" + std::string(name) +
                               "` is registered but section 11 does not list it; add it to "
                               "stylesheet-v1.properties.txt under its row, or to kEngineInternal "
                               "with the section that documents it") &&
             ok;
    }
    for (const std::string_view name : registry.shorthand_names()) {
        bool listed = false;
        for (const MatrixEntry& entry : entries) {
            if (entry.name == name && entry.kind == "shorthand") {
                listed = true;
                break;
            }
        }
        if (listed) {
            continue;
        }
        ok = expect(false, "`" + std::string(name) +
                               "` is a registered shorthand but section 11 does not list it; add "
                               "it to stylesheet-v1.properties.txt under its row") &&
             ok;
    }
    return ok;
}

// An allow-list entry that no longer describes reality is a failure, so the lists only shrink.
bool check_allow_lists(const PropertyRegistry& registry, const std::vector<MatrixEntry>& entries) {
    bool ok = true;
    for (const AllowEntry& gap : kKnownGaps) {
        bool promised = false;
        for (const MatrixEntry& entry : entries) {
            if (entry.name == gap.name) {
                promised = true;
                break;
            }
        }
        if (!promised) {
            ok = expect(false, "kKnownGaps lists `" + std::string(gap.name) +
                                   "`, which section 11 no longer promises; delete the entry") &&
                 ok;
            continue;
        }
        const bool implemented =
            registry.find(gap.name) != nullptr || registry.longhands(gap.name) != nullptr;
        if (implemented) {
            ok = expect(false, "kKnownGaps lists `" + std::string(gap.name) +
                                   "`, but the registry now implements it; delete the entry") &&
                 ok;
        }
    }
    for (const AllowEntry& internal : kEngineInternal) {
        if (registry.find(internal.name) == nullptr &&
            registry.longhands(internal.name) == nullptr) {
            ok = expect(false, "kEngineInternal lists `" + std::string(internal.name) +
                                   "`, which the registry no longer registers; delete the entry") &&
                 ok;
        }
    }
    return ok;
}

// The `border` shorthand is `<width> || <style> || <color>` on all four sides (section 7.4), so it
// expands through the same path as the logical border sides over twelve longhands.
bool check_border_shorthand(const PropertyRegistry& registry) {
    const PropertyExpansionResult result =
        registry.expand(Declaration{"border", "1px solid red", false, 7});
    if (!expect(result.diagnostics.empty(), "`border: 1px solid red` is clean")) {
        return false;
    }
    if (!expect(result.declarations.size() == 12,
                "`border` expands to the twelve physical border longhands")) {
        return false;
    }
    for (const Declaration& declaration : result.declarations) {
        const std::string_view property = declaration.property;
        const std::string_view expected = property.ends_with("-width")   ? "1px"
                                          : property.ends_with("-style") ? "solid"
                                                                         : "red";
        if (!expect(declaration.value == expected, "`border` sets `" + std::string(property) +
                                                       "` to `" + std::string(expected) + "`") ||
            !expect(declaration.offset == 7, "`border` keeps the declaration offset")) {
            return false;
        }
    }
    // An omitted component resets to its initial value, as a shorthand does.
    const PropertyExpansionResult style_only =
        registry.expand(Declaration{"border", "dashed", false, 0});
    if (!expect(style_only.diagnostics.empty(), "`border: dashed` is clean") ||
        !expect(style_only.declarations.size() == 12, "`border: dashed` expands to twelve")) {
        return false;
    }
    for (const Declaration& declaration : style_only.declarations) {
        const std::string_view property = declaration.property;
        const std::string_view expected = property.ends_with("-width")   ? "0"
                                          : property.ends_with("-style") ? "dashed"
                                                                         : "currentColor";
        if (!expect(declaration.value == expected, "`border: dashed` resets `" +
                                                       std::string(property) + "` to `" +
                                                       std::string(expected) + "`")) {
            return false;
        }
    }
    // The wiki theme writes `border: 1px solid var(--color-border)`: the reference takes the one
    // slot the definite components leave open, so the theme keeps working.
    const PropertyExpansionResult var_component =
        registry.expand(Declaration{"border", "1px solid var(--color-border)", false, 0});
    if (!expect(var_component.diagnostics.empty(),
                "`border` with one `var()` component is clean") ||
        !expect(var_component.declarations.size() == 12,
                "`border` with one `var()` component expands to twelve")) {
        return false;
    }
    for (const Declaration& declaration : var_component.declarations) {
        const std::string_view property = declaration.property;
        const std::string_view expected = property.ends_with("-width")   ? "1px"
                                          : property.ends_with("-style") ? "solid"
                                                                         : "var(--color-border)";
        if (!expect(declaration.value == expected,
                    "`border: 1px solid var(--color-border)` sets `" + std::string(property) +
                        "` to `" + std::string(expected) + "`")) {
            return false;
        }
    }
    // Two open slots make the reference ambiguous, so it is reported instead of guessed.
    const PropertyExpansionResult ambiguous =
        registry.expand(Declaration{"border", "1px var(--rest)", false, 0});
    if (!expect(ambiguous.declarations.empty(),
                "`border` with an ambiguous `var()` expands to nothing") ||
        !expect(ambiguous.diagnostics.size() == 1,
                "`border` with an ambiguous `var()` reports one diagnostic")) {
        return false;
    }
    // A `var()` that is the whole value reaches every longhand, as `padding: var(--gap)` does.
    const PropertyExpansionResult whole_var =
        registry.expand(Declaration{"border", "var(--all)", false, 0});
    if (!expect(whole_var.diagnostics.empty(), "`border: var(--all)` is clean") ||
        !expect(whole_var.declarations.size() == 12, "`border: var(--all)` expands to twelve")) {
        return false;
    }
    for (const Declaration& declaration : whole_var.declarations) {
        if (!expect(declaration.value == "var(--all)",
                    "`border: var(--all)` passes the reference to `" + declaration.property +
                        "`")) {
            return false;
        }
    }
    return true;
}

bool supports(std::string_view text, const PropertyRegistry& registry) {
    const auto parsed = parse_supports_condition(text);
    return parsed.ok && evaluate_supports(parsed.condition, registry);
}

// The acceptance criteria of issue #250: a stylesheet can gate on these three, so `@supports` must
// answer true for them. `aspect-ratio` is a registered longhand with its own grammar; `transition`
// and `animation` are names the animation module parses, registered so the cascade stores them.
bool check_supports(const PropertyRegistry& registry) {
    return expect(supports("(aspect-ratio: 1)", registry), "@supports (aspect-ratio: 1) is true") &&
           expect(supports("(aspect-ratio: 16 / 9)", registry),
                  "@supports (aspect-ratio: 16 / 9) is true") &&
           expect(supports("(transition: none)", registry),
                  "@supports (transition: none) is true") &&
           expect(supports("(animation: none)", registry), "@supports (animation: none) is true") &&
           expect(supports("(transition-property: opacity)", registry),
                  "@supports (transition-property: opacity) is true") &&
           expect(supports("(animation-name: fade)", registry),
                  "@supports (animation-name: fade) is true") &&
           expect(supports("(border: 1px solid red)", registry),
                  "@supports (border: 1px solid red) is true");
}

// The same three conditions through the whole pipeline: a `@supports` block that gates a fallback
// must actually apply its rule, which is what a stylesheet author observes.
bool check_supports_pipeline() {
    const auto sheet = compile_stylesheet(R"(
        .a { color: red; }
        @supports (aspect-ratio: 1) { .a { color: green; } }
        @supports (transition: none) { .a { opacity: 0.5; } }
        @supports (animation: none) { .a { width: 10px; } }
    )");
    StyleNode node;
    node.tag = "div";
    node.classes = {"a"};
    const ComputedStyle style = sheet.compute(node, nullptr);
    const auto value_is = [&style](const char* property, std::string_view expected) {
        const std::string* value = style.find(property);
        return value != nullptr && *value == expected;
    };
    return expect(sheet.diagnostics().empty(), "the @supports probe sheet is clean") &&
           expect(value_is("color", "green"), "@supports (aspect-ratio: 1) applies its rule") &&
           expect(value_is("opacity", "0.5"), "@supports (transition: none) applies its rule") &&
           expect(value_is("width", "10px"), "@supports (animation: none) applies its rule");
}

// The animation and transition longhands must stay absent until declared: `parse_style_animations`
// reads `animation-name` first and falls back to the `animation` shorthand, so an initial value
// here would make every node look animated.
bool check_animation_initials(const PropertyRegistry& registry) {
    constexpr const char* kNames[] = {
        "animation-name",
        "animation-duration",
        "animation-timing-function",
        "animation-delay",
        "animation-iteration-count",
        "animation-direction",
        "animation-fill-mode",
        "animation-play-state",
        "animation",
        "transition-property",
        "transition-duration",
        "transition-timing-function",
        "transition-delay",
        "transition",
    };
    for (const char* name : kNames) {
        const auto* info = registry.find(name);
        if (!expect(info != nullptr, "`" + std::string(name) + "` is registered") ||
            !expect(info->initial.empty(),
                    "`" + std::string(name) + "` has no initial value (absent until declared)") ||
            !expect(!info->inherited, "`" + std::string(name) + "` does not inherit")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    const PropertyRegistry registry;

    std::vector<MatrixEntry> entries;
    if (!load_matrix(entries)) {
        return 1;
    }

    bool ok = true;
    for (const MatrixEntry& entry : entries) {
        ok = check_promised(registry, entry) && ok;
    }
    ok = check_registered(registry, entries) && ok;
    ok = check_allow_lists(registry, entries) && ok;
    ok = check_border_shorthand(registry) && ok;
    ok = check_supports(registry) && ok;
    ok = check_supports_pipeline() && ok;
    ok = check_animation_initials(registry) && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "property_registry_matrix: " << entries.size()
              << " matrix entries agree with the registry\n";
    return 0;
}
