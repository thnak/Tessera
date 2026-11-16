// SPDX-License-Identifier: MIT
//
// Locale negotiation (docs/specs/i18n-v1.md sections 1 and 8.1). The chain is the spec's, in the
// spec's order; the supported set is explicit data so nothing here depends on the machine.

#include "tessera/appfw/i18n_locale.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>

namespace tessera::appfw {
namespace {

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

// The normalized language subtag (everything before the first '_'), or the whole tag.
std::string_view language_of(std::string_view tag) {
    const std::size_t separator = tag.find('_');
    return separator == std::string_view::npos ? tag : tag.substr(0, separator);
}

// The script subtag (four alphabetic characters), or empty when the tag has none. A script is what
// separates `zh_Hans` from `zh_Hant`, so the language+script step must not ignore it.
std::string_view script_of(std::string_view tag) {
    std::size_t start = tag.find('_');
    while (start != std::string_view::npos && start + 1 < tag.size()) {
        const std::size_t end = tag.find('_', start + 1);
        const std::string_view subtag = tag.substr(
            start + 1, end == std::string_view::npos ? std::string_view::npos : end - start - 1);
        if (subtag.size() == 4) {
            bool all_alpha = true;
            for (const char value : subtag) {
                all_alpha = all_alpha && is_alpha(value);
            }
            if (all_alpha) {
                return subtag;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end;
    }
    return {};
}

} // namespace

std::string normalize_locale_tag(std::string_view tag) {
    std::string normalized;
    normalized.reserve(tag.size());
    std::size_t start = 0;
    bool first = true;
    while (start <= tag.size()) {
        const std::size_t end = tag.find_first_of("-_", start);
        const std::string_view subtag =
            tag.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (subtag.empty()) {
            // An empty subtag (`en__US`, a trailing separator) is not a locale tag. Reject the
            // whole request rather than silently joining its neighbours into a tag nobody declared.
            return {};
        }
        if (!normalized.empty()) {
            normalized.push_back('_');
        }
        bool all_alpha = !subtag.empty();
        bool all_digit = !subtag.empty();
        for (const char value : subtag) {
            all_alpha = all_alpha && is_alpha(value);
            all_digit = all_digit && is_digit(value);
        }
        if (first) {
            for (const char value : subtag) {
                normalized.push_back(as_lower(value));
            }
            first = false;
        } else {
            for (std::size_t index = 0; index < subtag.size(); ++index) {
                const char value = subtag[index];
                if (all_alpha && subtag.size() == 2) {
                    normalized.push_back(as_upper(value)); // region
                } else if (all_alpha && subtag.size() == 4) {
                    normalized.push_back(index == 0 ? as_upper(value) : as_lower(value)); // script
                } else if (all_digit) {
                    normalized.push_back(value); // numeric variant
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
    if (normalized.empty() || !is_alpha(normalized.front())) {
        return {};
    }
    return normalized;
}

std::expected<LocaleTable, I18nError> LocaleTable::create(const LocaleTableConfig& config) {
    if (config.locales.empty()) {
        return std::unexpected(I18nError{I18nErrorCode::EmptyLocaleTable, {}});
    }
    if (config.locales.size() > kMaxLocales) {
        return std::unexpected(I18nError{I18nErrorCode::CapacityExceeded, {}});
    }

    LocaleTable table;
    table.entries_.reserve(config.locales.size());
    for (const SupportedLocale& locale : config.locales) {
        const std::string tag = normalize_locale_tag(locale.tag);
        if (tag.empty()) {
            return std::unexpected(
                I18nError{I18nErrorCode::DuplicateLocale, std::string(locale.tag)});
        }
        table.entries_.push_back(Entry{tag, locale.direction});
    }
    std::sort(table.entries_.begin(), table.entries_.end(),
              [](const Entry& left, const Entry& right) { return left.tag < right.tag; });
    for (std::size_t index = 1; index < table.entries_.size(); ++index) {
        if (table.entries_[index - 1].tag == table.entries_[index].tag) {
            return std::unexpected(
                I18nError{I18nErrorCode::DuplicateLocale, table.entries_[index].tag});
        }
    }

    table.default_locale_ = normalize_locale_tag(config.default_locale);
    if (table.find(table.default_locale_) == nullptr) {
        return std::unexpected(
            I18nError{I18nErrorCode::UnknownDefaultLocale, std::string(config.default_locale)});
    }
    table.fallback_locale_ = normalize_locale_tag(config.fallback_locale);
    if (!table.fallback_locale_.empty() && table.find(table.fallback_locale_) == nullptr) {
        return std::unexpected(
            I18nError{I18nErrorCode::UnknownFallbackLocale, std::string(config.fallback_locale)});
    }
    return table;
}

const LocaleTable::Entry* LocaleTable::find(std::string_view tag) const noexcept {
    if (tag.empty()) {
        return nullptr;
    }
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), tag,
        [](const Entry& entry, std::string_view value) { return entry.tag < value; });
    if (found == entries_.end() || found->tag != tag) {
        return nullptr;
    }
    return &*found;
}

LocaleResolution LocaleTable::resolve(std::span<const std::string_view> requests) const {
    LocaleResolution result;
    // The first request that normalized to anything: what a diagnostic names when *nothing* in the
    // list resolves. On a successful step `requested` is replaced by the request that served it, so
    // the two fields always explain each other.
    std::string first_request;
    for (const std::string_view request : requests) {
        const std::string normalized = normalize_locale_tag(request);
        if (normalized.empty()) {
            continue;
        }
        if (first_request.empty()) {
            first_request = normalized;
        }

        // 1. exact
        if (const Entry* entry = find(normalized); entry != nullptr) {
            result.requested = normalized;
            result.resolved = entry->tag;
            result.direction = entry->direction;
            result.match = LocaleMatch::Exact;
            return result;
        }

        // 2. language + script
        const std::string_view language = language_of(normalized);
        const std::string_view script = script_of(normalized);
        if (!script.empty()) {
            std::string language_script(language);
            language_script.push_back('_');
            language_script.append(script);
            if (const Entry* entry = find(language_script); entry != nullptr) {
                result.requested = normalized;
                result.resolved = entry->tag;
                result.direction = entry->direction;
                result.match = LocaleMatch::LanguageScript;
                return result;
            }
        }

        // 3. language
        if (const Entry* entry = find(language); entry != nullptr) {
            result.requested = normalized;
            result.resolved = entry->tag;
            result.direction = entry->direction;
            result.match = LocaleMatch::Language;
            return result;
        }
    }
    result.requested = first_request;

    // 4. fallback_locale, then 5. default_locale. Both are table members validated by create(), so
    // neither step can produce an empty tag: "Unsupported locale -> default locale + diagnostic,
    // never empty strings".
    if (const Entry* entry = find(fallback_locale_); entry != nullptr) {
        result.resolved = entry->tag;
        result.direction = entry->direction;
        result.match = LocaleMatch::Fallback;
        return result;
    }
    if (const Entry* entry = find(default_locale_); entry != nullptr) {
        result.resolved = entry->tag;
        result.direction = entry->direction;
        result.match = LocaleMatch::Default;
    }
    return result;
}

LocaleResolution LocaleTable::resolve(std::string_view request) const {
    const std::string_view requests[] = {request};
    return resolve(requests);
}

bool LocaleTable::contains(std::string_view tag) const {
    return find(normalize_locale_tag(tag)) != nullptr;
}

SupportedLocale LocaleTable::at(std::size_t index) const {
    if (index >= entries_.size()) {
        return {};
    }
    return SupportedLocale{entries_[index].tag, entries_[index].direction};
}

} // namespace tessera::appfw
