// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <string_view>

namespace tessera::ui::style {

// Tessera's user-agent stylesheet: the defaults an element has before any author CSS, chosen to
// match what a browser's own user-agent stylesheet does for the same element, so a screen behaves
// the same in Tessera and in a browser until the author says otherwise (ADR-063,
// docs/specs/stylesheet-v1.md section 7.1).
//
// **The goal is the browser's default look, as far as it is possible.** A difference from a browser
// is an engine bug unless it is impossible, and each impossible case is a named exception
// (ADR-063). The sheet grows one rule at a time: a rule is added when a difference is found, names
// the browser behaviour it mirrors, and has a test that pins it. Every rule is `Origin::UserAgent`,
// which ranks below every other origin for a normal declaration, so any author, theme or inline
// declaration overrides it without a specificity contest.
[[nodiscard]] std::string_view ua_stylesheet_text() noexcept;

// The sheet prepared once (media independent) and shared by every cascade that includes it: the app
// host's and the design-time preview's, so the two cannot disagree about a default. Append it with
// `Origin::UserAgent`.
[[nodiscard]] const PreparedStyleSheet& ua_prepared_stylesheet();

} // namespace tessera::ui::style
