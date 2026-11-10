// SPDX-License-Identifier: MIT

#include "icu_locale_manifest.h" // generated kept-locale table
#include "icu_runtime.hpp"
#include "locale_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include <unicode/uloc.h>
#include <unicode/utypes.h>

namespace tessera::ui::text {
namespace {

constexpr std::string_view kRoot = "root";

bool is_alpha(char value) {
    return std::isalpha(static_cast<unsigned char>(value)) != 0;
}

bool is_digit(char value) {
    return std::isdigit(static_cast<unsigned char>(value)) != 0;
}

char as_lower(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

char as_upper(char value) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
}

// Reduced canonical form: '-' separators become '_', the language is lowercased, two-letter
// subtags are uppercased (region), and four-letter subtags are title-cased (script). Anything
// else is left alone. This is the documented V1 rule; it is not full CLDR canonicalization.
std::string normalize(std::string_view locale) {
    std::string normalized;
    normalized.reserve(locale.size());
    std::size_t start = 0;
    bool first = true;
    while (start <= locale.size()) {
        const std::size_t end = locale.find_first_of("-_", start);
        const std::string_view subtag = locale.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!normalized.empty()) {
            normalized.push_back('_');
        }
        if (first) {
            for (const char value : subtag) {
                normalized.push_back(as_lower(value));
            }
            first = false;
        } else {
            bool all_alpha = !subtag.empty();
            for (const char value : subtag) {
                all_alpha = all_alpha && is_alpha(value);
            }
            bool all_digit = !subtag.empty();
            for (const char value : subtag) {
                all_digit = all_digit && is_digit(value);
            }
            for (std::size_t index = 0; index < subtag.size(); ++index) {
                const char value = subtag[index];
                if (all_alpha && subtag.size() == 2) {
                    normalized.push_back(as_upper(value));
                } else if (all_alpha && subtag.size() == 4) {
                    normalized.push_back(index == 0 ? as_upper(value) : as_lower(value));
                } else if (all_digit) {
                    normalized.push_back(value);
                } else {
                    normalized.push_back(as_lower(value));
                }
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return normalized;
}

bool manifest_contains(std::string_view locale) {
    for (int index = 0; index < icu::kKeptLocaleCount; ++index) {
        if (locale == icu::kKeptLocales[index]) {
            return true;
        }
    }
    return false;
}

// Nearest kept ancestor: de_AT_CH -> de_AT -> de -> root.
std::string nearest_kept(std::string_view locale) {
    std::string candidate(locale);
    while (true) {
        if (manifest_contains(candidate)) {
            return candidate;
        }
        const std::size_t cut = candidate.find_last_of('_');
        if (cut == std::string::npos) {
            break;
        }
        candidate.erase(cut);
    }
    return manifest_contains(kRoot) ? std::string(kRoot) : std::string();
}

std::string_view default_locale() {
    return manifest_contains("en") ? std::string_view("en") : kRoot;
}

bool is_right_to_left(std::string_view locale) {
    if (!icu::ready() && icu::initialize() == icu::InitStatus::Failed) {
        return false;
    }
    UErrorCode status = U_ZERO_ERROR;
    const std::string id(locale);
    const ULayoutType orientation = uloc_getCharacterOrientation(id.c_str(), &status);
    return U_SUCCESS(status) && orientation == ULOC_LAYOUT_RTL;
}

} // namespace

std::string LocaleResolver::fallback_for(std::string_view locale) {
    const std::string normalized = normalize(locale);
    if (manifest_contains(normalized)) {
        return {};
    }
    return nearest_kept(normalized);
}

LocaleResolution LocaleResolver::resolve(std::string_view requested) {
    LocaleResolution result;
    result.requested = normalize(requested);

    if (!icu::ready() && icu::initialize() == icu::InitStatus::Failed) {
        result.diagnostic = "icu.locale.unavailable";
    }

    if (manifest_contains(result.requested)) {
        result.kept = true;
        result.resolved = result.requested;
    } else {
        const std::string kept = nearest_kept(result.requested);
        if (kept.empty()) {
            result.resolved = std::string(default_locale());
            result.fallback = result.resolved;
            result.diagnostic = "icu.locale.missing requested=" + result.requested;
        } else {
            result.resolved = kept;
            result.fallback = kept;
            result.diagnostic =
                "icu.locale.fallback requested=" + result.requested + " resolved=" + kept;
        }
    }
    result.right_to_left = is_right_to_left(result.resolved);
    return result;
}

bool LocaleResolver::is_kept(std::string_view locale) {
    return manifest_contains(normalize(locale));
}

std::size_t LocaleResolver::kept_locale_count() {
    return static_cast<std::size_t>(icu::kKeptLocaleCount);
}

std::string_view LocaleResolver::kept_locale(std::size_t index) {
    if (index >= kept_locale_count()) {
        return {};
    }
    return icu::kKeptLocales[index];
}

} // namespace tessera::ui::text
