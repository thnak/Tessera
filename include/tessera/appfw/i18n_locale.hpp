// SPDX-License-Identifier: MIT
//
// Locale negotiation for the i18n module (docs/specs/i18n-v1.md sections 1, 8.1 and 9; ADR-050
// section 8, row `i18n`). Issue #195.
//
// The spec fixes the *chain* and leaves the *input shape* open, so this header makes the two
// decisions the chain needs and states them:
//
//   1. **The supported set is explicit data, never a probe of the machine.** `LocaleTable` is built
//      from a compiled-in list of `{tag, direction}` pairs (section 8.1: "ICU data is ... pinned;
//      locale packs are bundled"). Nothing here reads an OS locale, calls `setlocale`, or touches
//      `std::locale`. That is the spec's "deterministic mode pins locale" applied to *every* build
//      rather than to a test mode: a golden or a unit test cannot drift with the machine, which is
//      also what the acceptance ("tests are deterministic") asks for.
//
//   2. **The requests are an ordered list, not one string.** Section 1 names four inputs (OS
//      locale(s), app default, user override, per-window override) and section 8.1 shows a single
//      `requested -> normalize -> ...` chain. The list is how the first is spelled without an OS
//      call: the composition root passes `{user override, app default, OS locales...}` in priority
//      order, and the table walks the documented chain once per request. A caller with one request
//      passes a one-element list.
//
// Normalization is the *reduced* canonical form `docs/design/icu-data.md` and
// `src/ui/text/locale_resolver.cpp` already use - `-` becomes `_`, the language is lowercased, a
// two-letter region is uppercased and a four-letter script is title-cased - and deliberately not
// full CLDR canonicalization. Reusing that rule keeps one spelling of a locale tag in the tree; the
// ICU-backed resolver can replace this table later without changing a caller.

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

// The base direction a locale seeds (i18n-v1.md section 4: "the locale's base direction seeds the
// inherited `direction` property"). It is carried by the supported-locale entry rather than derived
// from ICU so the module builds and tests without the optional ICU data pack
// (`TESSERA_ICU_REDUCED_DATA`, docs/design/icu-data.md section 4). The field a real ICU-backed
// table would fill is the same one, so wiring it later is a data change, not an API change.
enum class TextDirection : std::uint8_t {
    LeftToRight,
    RightToLeft,
};

[[nodiscard]] constexpr std::string_view text_direction_name(TextDirection direction) noexcept {
    switch (direction) {
    case TextDirection::LeftToRight:
        return "ltr";
    case TextDirection::RightToLeft:
        return "rtl";
    }
    return "ltr";
}

// Why a locale table or a message table could not be built. Configuration errors are values, like
// every other boundary in the framework (ADR-025, ADR-050 section 4 item 5), and they are reported
// at composition time, never on a frame.
enum class I18nErrorCode : std::uint8_t {
    EmptyLocaleTable,
    CapacityExceeded,
    DuplicateLocale,
    UnknownDefaultLocale,
    UnknownFallbackLocale,
    EmptyCatalog,
    DuplicateMessageId,
    MissingDefaultMessage,
    // A message names a locale the `LocaleTable` does not support, so its pattern could never be
    // selected. Distinct from `UnknownDefaultLocale`: this one is about a message, not the table.
    UnsupportedLocale,
};

[[nodiscard]] constexpr std::string_view i18n_error_code_name(I18nErrorCode code) noexcept {
    switch (code) {
    case I18nErrorCode::EmptyLocaleTable:
        return "empty locale table";
    case I18nErrorCode::CapacityExceeded:
        return "capacity exceeded";
    case I18nErrorCode::DuplicateLocale:
        return "duplicate locale";
    case I18nErrorCode::UnknownDefaultLocale:
        return "unknown default locale";
    case I18nErrorCode::UnknownFallbackLocale:
        return "unknown fallback locale";
    case I18nErrorCode::EmptyCatalog:
        return "empty catalog";
    case I18nErrorCode::DuplicateMessageId:
        return "duplicate message id";
    case I18nErrorCode::MissingDefaultMessage:
        return "missing default message";
    case I18nErrorCode::UnsupportedLocale:
        return "unsupported locale";
    }
    return "unknown";
}

struct I18nError {
    I18nErrorCode code = I18nErrorCode::EmptyLocaleTable;
    // The offending tag or message id, owned so an error can outlive the table it came from.
    std::string subject;

    [[nodiscard]] bool operator==(const I18nError&) const = default;
};

// The reduced canonical form described in the header comment. An empty or unparseable request
// normalizes to the empty string, which `LocaleTable::resolve` treats as "no request".
[[nodiscard]] std::string normalize_locale_tag(std::string_view tag);

// One supported locale: its canonical tag plus the direction its data implies.
struct SupportedLocale {
    std::string_view tag;
    TextDirection direction = TextDirection::LeftToRight;
};

// The step that produced the resolution, i.e. the chain of i18n-v1.md section 8.1:
//
//   requested -> normalize -> exact -> language+script -> language -> fallback -> default
//
// `Exact` is the only step that means the request itself is served; every other step is a fallback
// the spec requires to be deterministic, and the caller (or a diagnostic) may want to know which.
enum class LocaleMatch : std::uint8_t {
    Exact,
    LanguageScript,
    Language,
    Fallback,
    Default,
};

[[nodiscard]] constexpr std::string_view locale_match_name(LocaleMatch match) noexcept {
    switch (match) {
    case LocaleMatch::Exact:
        return "exact";
    case LocaleMatch::LanguageScript:
        return "language+script";
    case LocaleMatch::Language:
        return "language";
    case LocaleMatch::Fallback:
        return "fallback";
    case LocaleMatch::Default:
        return "default";
    }
    return "unknown";
}

struct LocaleResolution {
    // Normalized first request that was considered (empty when every request was empty).
    std::string requested;
    // Normalized tag actually used. Never empty once the table exists: a table always has a
    // default.
    std::string resolved;
    LocaleMatch match = LocaleMatch::Default;
    TextDirection direction = TextDirection::LeftToRight;

    // True when the request itself is served rather than a fallback of it.
    [[nodiscard]] bool exact() const noexcept {
        return match == LocaleMatch::Exact;
    }
    // True when the resolution is the request or a sub-tag of it (`de_AT` -> `de`).
    [[nodiscard]] bool within_requested_language() const noexcept {
        return match == LocaleMatch::Exact || match == LocaleMatch::LanguageScript ||
               match == LocaleMatch::Language;
    }
};

struct LocaleTableConfig {
    std::span<const SupportedLocale> locales;
    // Terminal step after every request misses. May be empty, in which case the chain goes straight
    // to `default_locale` (i18n-v1.md section 8.1 lists both steps).
    std::string_view fallback_locale;
    // Required: "Unsupported locale -> default locale + diagnostic, never empty strings".
    std::string_view default_locale;
};

// The compiled-in supported set, normalized and sorted for a binary search.
//
// Construction validates the table instead of trusting it: an empty table, a duplicate tag, a tag
// that normalizes to nothing, or a default/fallback that is not supported is an `I18nError` rather
// than a locale nobody can serve (i18n-v1.md section 6: "unused/missing/duplicate keys fail CI" is
// the build-time version of the same rule; a module handed a bad table at runtime reports it).
class LocaleTable {
public:
    // Bounded so the module never grows with the locale count of an arbitrary host; the catalog is
    // a build artifact and V1 packages "only needed locales" (i18n-v1.md section 6).
    static constexpr std::size_t kMaxLocales = 64;

    [[nodiscard]] static std::expected<LocaleTable, I18nError>
    create(const LocaleTableConfig& config);

    // Walks the section 8.1 chain for each request in order and returns the first request that
    // reaches a supported locale. The result is a pure function of (this table, requests): no
    // process state is read, so two calls with the same arguments always agree.
    [[nodiscard]] LocaleResolution resolve(std::span<const std::string_view> requests) const;

    // Convenience for the single-request callers (a user override, a per-window override).
    [[nodiscard]] LocaleResolution resolve(std::string_view request) const;

    [[nodiscard]] bool contains(std::string_view tag) const;

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }
    [[nodiscard]] const std::string& default_locale() const noexcept {
        return default_locale_;
    }
    [[nodiscard]] const std::string& fallback_locale() const noexcept {
        return fallback_locale_;
    }
    // Sorted by tag; the index form is what the tests and a settings UI enumerate.
    [[nodiscard]] SupportedLocale at(std::size_t index) const;

private:
    struct Entry {
        std::string tag;
        TextDirection direction = TextDirection::LeftToRight;
    };

    [[nodiscard]] const Entry* find(std::string_view tag) const noexcept;

    std::vector<Entry> entries_;
    std::string default_locale_;
    std::string fallback_locale_;
};

} // namespace tessera::appfw
