// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {

// The document-global values `env(<name> [, <fallback>])` reads (ADR-074 section 2). Each value is
// CSS text, already in the form it substitutes as (`12px`, `1.5`, `0px`), so the resolver stays a
// textual substitution like `var()` and the host owns every unit decision.
//
// A name is the variable's identifier plus its integer arguments, single-space separated:
// `safe-area-inset-top`, `viewport-segment-width 0 1`. Names are case-sensitive, as the standard
// has them. A variable that is absent is the standard's "variable does not exist": the fallback
// applies, and without one the declaration is invalid at computed-value time.
class StyleEnvironment {
public:
    void set(std::string name, std::string value);
    [[nodiscard]] const std::string* find(std::string_view name) const;
    [[nodiscard]] bool empty() const {
        return entries_.empty();
    }
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const {
        return entries_;
    }
    // Same names, same values. The comparison a host makes before re-resolving a cascade.
    [[nodiscard]] bool operator==(const StyleEnvironment& other) const = default;

private:
    std::vector<std::pair<std::string, std::string>> entries_; // sorted by name
};

// One logical region of the viewport (Media Queries 5: a region split off by a hardware feature).
struct EnvironmentSegment {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// What the standard variables are computed from. All lengths are CSS px. A window with no
// separating fold has one segment, the whole viewport.
struct StandardEnvironmentInputs {
    float safe_area_top = 0.0f;
    float safe_area_right = 0.0f;
    float safe_area_bottom = 0.0f;
    float safe_area_left = 0.0f;
    float text_scale = 1.0f;
    // The segment grid, row-major: `segments_x * segments_y` rectangles. Empty means the single
    // whole-viewport segment given by `viewport_width` and `viewport_height`.
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
    std::size_t segments_x = 1;
    std::size_t segments_y = 1;
    std::vector<EnvironmentSegment> segments;
};

// The closed list of CSS Environment Variables Level 1 names the engine answers (ADR-074 section
// 3): `safe-area-inset-*`, `viewport-segment-{width,height,top,left,bottom,right} <x> <y>` and
// `preferred-text-scale`. Nothing else is defined here; the `--tessera-` names are a separate
// slice.
[[nodiscard]] StyleEnvironment standard_environment(const StandardEnvironmentInputs& inputs);

// The variable names a declaration value reads through `env()`, in order of appearance, including
// ones inside a fallback. Strings are skipped.
void collect_env_references(std::string_view value, std::vector<std::string>& names);

} // namespace tessera::ui::style
