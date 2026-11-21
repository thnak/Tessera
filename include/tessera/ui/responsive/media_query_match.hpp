// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/media_query.hpp"

#include <string>
#include <string_view>

namespace tessera::ui::responsive {

// `useMediaQuery(query)` (ADR-064 item 7, issue #631): a reactive boolean for one media query,
// evaluated by the **stylesheet's own** evaluator.
//
// **Why this is a function and not a second evaluator.** The rule the issue states is "consistent
// with `useBreakpoint` and the stylesheet's media evaluation (same engine path, no second
// evaluator)". A hook that parsed `(min-width: 600px)` its own way would drift from the cascade the
// moment either side gained a feature, and the drift would show up as a component that renders its
// wide layout while its `@media` rule still applies the narrow one. So the hook calls
// `style::parse_media_query_list` and `style::evaluate` — the same two functions the cascade calls
// — and nothing else.
//
// **Header-only on purpose**, for the same reason `size_class.hpp` is: the preview session and
// every canvas test target compile the session sources directly rather than linking a library.
//
// A query that does not parse is `false`, not an error: a media query is a string a component may
// build at runtime, and a malformed one must not take the frame down. The parse result is cached by
// the caller (the generated base holds one `MediaQueryList` per hook), so a per-frame evaluation is
// one walk over the parsed list.
class MediaQueryMatcher {
public:
    // Parses `query` once. Returns false when the text is not a media query list, in which case
    // `matches` is always false.
    bool parse(std::string_view query) {
        const style::MediaQueryParseResult parsed = style::parse_media_query_list(query);
        if (!parsed.ok) {
            queries_ = {};
            valid_ = false;
            return false;
        }
        queries_ = parsed.list;
        valid_ = true;
        return true;
    }

    // Evaluates against `context`, the same media context the cascade resolves with.
    [[nodiscard]] bool matches(const style::MediaContext& context) const {
        return valid_ && style::evaluate(queries_, context);
    }

    [[nodiscard]] bool valid() const {
        return valid_;
    }

private:
    style::MediaQueryList queries_{};
    bool valid_ = false;
};

} // namespace tessera::ui::responsive
