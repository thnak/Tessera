// SPDX-License-Identifier: MIT
//
// The pieces the `@media` parser and evaluator share (issue #691). The parser lives in
// `media_query_parse.cpp` and the evaluator in `media_query.cpp`; both need the feature-name
// mapping and the comparison operators, so those are declared here rather than duplicated.

#pragma once

#include "tessera/ui/style/media_query.hpp"

#include <string_view>

namespace tessera::ui::style {

// Maps a feature name (with any `min-`/`max-` prefix already stripped) to its enum. False for a
// name the engine does not implement, which keeps the documented behaviour: the query is false and
// a diagnostic names it, rather than silently accepting.
[[nodiscard]] bool feature_from_name(std::string_view name, MediaFeature& feature);

// True for the features that take a `<length>` or a ratio and therefore support the range form.
[[nodiscard]] bool is_range_feature(MediaFeature feature);

// The operator of a range comparison, or false when the text is not one.
[[nodiscard]] bool comparison_from_operator(std::string_view text, MediaComparison& comparison);

// `600px <= width` is `width >= 600px`: swapping the sides flips the operator.
[[nodiscard]] MediaComparison flip_comparison(MediaComparison comparison);

} // namespace tessera::ui::style
