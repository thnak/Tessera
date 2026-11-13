// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

// The supported CSS Grid value grammar (docs/specs/grid-v1.md). One parser serves the declaration
// validator, `@supports` and the layout engine, so all three agree on what V1 accepts. The grammar
// is deliberately small: fixed lengths, percentages, `fr`, `auto`, `min-content`/`max-content`,
// `minmax()` with a non-flexible minimum, and `repeat(<integer>, ...)`.
enum class GridTrackKind : std::uint8_t {
    Fixed,
    Percent,
    Fr,
    Auto,
    MinContent,
    MaxContent,
};

// The length unit a fixed track size carries. `None` means the size is not a length.
enum class GridLengthUnit : std::uint8_t {
    None,
    Px,
    Rem,
    Em,
    Vw,
    Vh,
    Pt,
};

struct GridTrackSize {
    GridTrackKind kind = GridTrackKind::Auto;
    float value = 0.0f;
    GridLengthUnit unit = GridLengthUnit::None;
};

// A track is `minmax(min, max)`. A plain track size `X` becomes `minmax(X, X)` for a fixed or
// percentage size and `minmax(auto, X)` otherwise, as in CSS Grid.
struct GridTrack {
    GridTrackSize min;
    GridTrackSize max;
};

struct GridTemplate {
    std::vector<GridTrack> tracks;
    // False when the declaration is not in the supported grammar; the caller then treats the
    // container as having no explicit tracks (every track is an implicit `auto` track).
    bool valid = false;
};

// Parses `grid-template-columns`/`grid-template-rows`. `none` is valid and yields no tracks.
[[nodiscard]] GridTemplate parse_grid_template(std::string_view value);

// One axis of an item's `grid-column`/`grid-row`. `start`/`end` are 1-based line numbers (a
// negative number counts back from the end of the explicit grid); `0` means `auto`. `span` is used
// when the other side is `auto`.
struct GridLine {
    int start = 0;
    int end = 0;
    int span = 1;
    bool valid = false;
};

[[nodiscard]] GridLine parse_grid_line(std::string_view value);

// Parses a single `<length-percentage>` token (used for `gap`/`row-gap`/`column-gap`). Returns
// false for `normal`, `auto` and anything outside the supported unit set.
[[nodiscard]] bool parse_grid_length(std::string_view value, GridTrackSize& out);

// `grid-auto-flow`: `row` or `column`. `dense` is not supported in V1.
[[nodiscard]] bool is_supported_grid_auto_flow(std::string_view value);

// `justify-items`/`justify-self`/`align-items`/`align-self` keywords V1 understands for grid.
// `baseline` and the `safe`/`unsafe`/`legacy` prefixes are not supported (`legacy` is accepted as
// the registered initial value of `justify-items` and behaves as `normal`).
[[nodiscard]] bool is_supported_grid_alignment(std::string_view value);

} // namespace tessera::ui::style
