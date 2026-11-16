// SPDX-License-Identifier: MIT
//
// The compiled message catalog (docs/specs/i18n-v1.md sections 2 and 8.1). No parsing, no I/O: the
// table is data a code generator emits, and lookups are a binary search over (id, locale).

#include "tessera/appfw/i18n_catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::appfw {
namespace {

std::string_view language_of(std::string_view tag) {
    const std::size_t separator = tag.find('_');
    return separator == std::string_view::npos ? tag : tag.substr(0, separator);
}

bool entry_less(const std::string& id, const std::string& locale, const std::string& other_id,
                const std::string& other_locale) {
    return id < other_id || (id == other_id && locale < other_locale);
}

} // namespace

std::expected<MessageCatalog, I18nError> MessageCatalog::create(const MessageCatalogConfig& config,
                                                                const LocaleTable& locales) {
    if (config.messages.empty()) {
        return std::unexpected(I18nError{I18nErrorCode::EmptyCatalog, {}});
    }
    if (config.messages.size() > kMaxMessages) {
        return std::unexpected(I18nError{I18nErrorCode::CapacityExceeded, {}});
    }
    const std::string default_locale = normalize_locale_tag(config.default_locale);
    if (!locales.contains(default_locale)) {
        return std::unexpected(
            I18nError{I18nErrorCode::UnknownDefaultLocale, std::string(config.default_locale)});
    }

    MessageCatalog catalog;
    catalog.default_locale_ = default_locale;
    catalog.fallback_locale_ = locales.fallback_locale();
    catalog.diagnostic_capacity_ = config.diagnostic_capacity;
    catalog.entries_.reserve(config.messages.size());
    for (const MessageEntry& message : config.messages) {
        const std::string locale = normalize_locale_tag(message.locale);
        if (!locales.contains(locale)) {
            return std::unexpected(
                I18nError{I18nErrorCode::UnsupportedLocale, std::string(message.locale)});
        }
        if (message.id.empty()) {
            return std::unexpected(I18nError{I18nErrorCode::DuplicateMessageId, {}});
        }
        catalog.entries_.push_back(
            Entry{std::string(message.id), locale, std::string(message.pattern.text)});
    }
    std::sort(catalog.entries_.begin(), catalog.entries_.end(),
              [](const Entry& left, const Entry& right) {
                  return entry_less(left.id, left.locale, right.id, right.locale);
              });
    for (std::size_t index = 1; index < catalog.entries_.size(); ++index) {
        const Entry& previous = catalog.entries_[index - 1];
        const Entry& current = catalog.entries_[index];
        if (previous.id == current.id && previous.locale == current.locale) {
            return std::unexpected(I18nError{I18nErrorCode::DuplicateMessageId, current.id});
        }
    }

    // Every id must exist in the default locale (section 8.1). One pass over the sorted table: the
    // current id group is complete only when its default-locale entry is present.
    std::vector<std::string_view> locale_tags;
    locale_tags.reserve(catalog.entries_.size());
    for (std::size_t index = 0; index < catalog.entries_.size();) {
        const std::string& id = catalog.entries_[index].id;
        bool has_default = false;
        std::size_t next = index;
        while (next < catalog.entries_.size() && catalog.entries_[next].id == id) {
            if (catalog.entries_[next].locale == default_locale) {
                has_default = true;
            }
            if (std::find(locale_tags.begin(), locale_tags.end(), catalog.entries_[next].locale) ==
                locale_tags.end()) {
                locale_tags.push_back(catalog.entries_[next].locale);
            }
            ++next;
        }
        if (!has_default) {
            return std::unexpected(I18nError{I18nErrorCode::MissingDefaultMessage, id});
        }
        index = next;
    }
    catalog.locale_count_ = locale_tags.size();
    return catalog;
}

MessageCatalog::Chain MessageCatalog::chain_for(std::string_view resolved_locale) const noexcept {
    Chain chain;
    const auto push = [&chain](std::string_view step) {
        if (step.empty()) {
            return;
        }
        for (std::size_t index = 0; index < chain.count; ++index) {
            if (chain.steps[index] == step) {
                return;
            }
        }
        if (chain.count < 4) {
            chain.steps[chain.count++] = step;
        }
    };
    const std::string_view normalized = resolved_locale;
    push(normalized);
    push(language_of(normalized));
    push(fallback_locale_);
    push(default_locale_);
    return chain;
}

const MessageCatalog::Entry* MessageCatalog::find(std::string_view id,
                                                  std::string_view locale) const noexcept {
    // Heterogeneous lower_bound: the entries are sorted by (id, locale), so the first entry not
    // "less than" the key is the only candidate. No temporary string, no allocation.
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), id, [locale](const Entry& entry, std::string_view key) {
            return entry.id < key || (entry.id == key && entry.locale < locale);
        });
    if (found == entries_.end() || found->id != id || found->locale != locale) {
        return nullptr;
    }
    return &*found;
}

std::string_view MessageCatalog::text(std::string_view id, std::string_view resolved_locale) const {
    const Chain chain = chain_for(resolved_locale);
    for (std::size_t index = 0; index < chain.count; ++index) {
        const Entry* entry = find(id, chain.steps[index]);
        if (entry == nullptr) {
            continue;
        }
        const bool from_default = entry->locale == default_locale_;
        if (index != 0) {
            record_miss(id, resolved_locale, entry->locale);
            if (from_default) {
                ++default_fallbacks_;
            }
        }
        return entry->text;
    }

    // "Missing key -> default locale + visible key/diagnostic in dev; never crash." The default
    // locale was part of the chain, so this is an id the catalog never had: the visible key is the
    // only text that is safe to show, and it is what identifies the mistake on screen.
    record_miss(id, resolved_locale, {});
    ++visible_keys_;
    return id;
}

bool MessageCatalog::contains(std::string_view id) const {
    return find(id, default_locale_) != nullptr;
}

void MessageCatalog::record_miss(std::string_view id, std::string_view resolved_locale,
                                 std::string_view served_locale) const {
    ++misses_;
    if (diagnostic_capacity_ == 0) {
        return;
    }
    if (diagnostics_.size() >= diagnostic_capacity_) {
        diagnostics_.erase(diagnostics_.begin());
        ++dropped_;
    }
    diagnostics_.push_back(MessageDiagnostic{std::string(id), std::string(resolved_locale),
                                             std::string(served_locale)});
}

MessageDiagnostics MessageCatalog::diagnostics() const {
    MessageDiagnostics result;
    result.capacity = diagnostic_capacity_;
    result.entries = diagnostics_;
    result.dropped = dropped_;
    result.misses = misses_;
    result.default_fallbacks = default_fallbacks_;
    result.visible_keys = visible_keys_;
    return result;
}

void MessageCatalog::clear_diagnostics() {
    diagnostics_.clear();
    dropped_ = 0;
    misses_ = 0;
    default_fallbacks_ = 0;
    visible_keys_ = 0;
}

} // namespace tessera::appfw
