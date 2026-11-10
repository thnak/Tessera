// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Locale negotiation for the text stack (docs/specs/i18n-v1.md section 8.1).
//
// The reduced ICU data package only carries the documented locale set, and ICU resolves anything
// else through the fallback chain to root *without an error code* (docs/design/icu-data.md
// section 5: `udat_open` returns U_ZERO_ERROR while formatting root data). The resolver therefore
// validates the request against the kept-locale manifest that the data step generates, so a
// missing pack becomes a diagnostic instead of silently wrong dates and numbers.
namespace tessera::ui::text {

enum class TextDirection { LeftToRight, RightToLeft };

struct LocaleResolution {
    std::string requested; // normalized request ("pt_BR", not "pt-br")
    std::string resolved;  // locale ICU will use, after the fallback chain
    std::string fallback;  // kept ancestor that supplied the data; empty when exact
    bool kept = false;     // the requested locale's own bundle is in the manifest
    bool right_to_left = false;
    // Non-empty when the request is not served by its own bundle. Stable, bounded, and safe to log:
    // "icu.locale.fallback requested=de_AT resolved=de" or "icu.locale.missing requested=zz".
    std::string diagnostic;
};

// Stateless locale negotiation against the linked manifest. Compiling this header without the ICU
// slice is not supported: the whole translation unit is part of the ICU block.
class LocaleResolver {
public:
    // Case- and separator-insensitive (BCP-47 "pt-BR", "PT_br" -> "pt_BR"). An unparseable or
    // unknown request resolves to "en" (the documented default) with a missing diagnostic.
    [[nodiscard]] static LocaleResolution resolve(std::string_view requested);

    // Exact manifest membership (after normalization).
    [[nodiscard]] static bool is_kept(std::string_view locale);

    // Locale bundles the linked blob carries, in the manifest's sorted order.
    [[nodiscard]] static std::size_t kept_locale_count();
    [[nodiscard]] static std::string_view kept_locale(std::size_t index);

    // The fallback chain a request would walk, excluding the request itself: de_AT -> de -> root.
    // Empty when the exact bundle is kept.
    [[nodiscard]] static std::string fallback_for(std::string_view locale);
};

} // namespace tessera::ui::text
