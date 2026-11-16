// SPDX-License-Identifier: MIT
//
// The message catalog (docs/specs/i18n-v1.md sections 2 and 8.1; issue #195).
//
// The spec's V1 format is "JSON with ICU MessageFormat strings, one file per locale, stable message
// ids", validated and compiled at build time into "a generated C++ table (id enum + pattern data)
// or a compact binary catalog", with "no source parsing" and "fallback is compiled in" at runtime.
// `MessageTable` is that generated table's shape: sorted, owning, and built by
// `MessageCatalog::create` from in-memory data a code generator emits. Nothing here parses JSON,
// reads a file, or fetches anything.
//
// Scope of this slice, stated because "ICU MessageFormat" is larger than it looks (report section:
// what is deferred):
//
//   * **Implemented**: stable ids, per-locale lookup, the deterministic fallback chain across the
//     locales the `LocaleTable` resolves (`resolved -> language -> fallback -> default`),
//     missing-key behaviour ("default locale + visible key/diagnostic in dev; never crash"), and
//     duplicate / missing-id validation at construction.
//   * **Deferred**: ICU MessageFormat itself - `{count, plural, ...}`, `select`, nested
//     placeholders, number/date skeletons. A pattern is returned verbatim, so a `{count, plural,
//     one {# order} other {# orders}}` string would be shown literally, not formatted. That is
//     deliberate: formatting needs the ICU data pack (`TESSERA_ICU_REDUCED_DATA` is OFF in the
//     default build and CI, docs/design/icu-data.md section 4), and inventing a second plural
//     engine in the meantime is exactly the "string table bolted on at the end" the spec opens by
//     rejecting. The parse seam is `MessagePattern`, so a compiled plural AST can replace the
//     verbatim string without touching a caller.
//
// Keys are `string_view`s that the caller owns (a generated table has static storage). The catalog
// copies them once at construction so a key built on the stack cannot dangle.

#pragma once

#include "tessera/appfw/i18n_locale.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

// One compiled message. Today the pattern is the literal (already compiled) string; a parsed form
// lands with ICU MessageFormat, and this type is where it goes.
struct MessagePattern {
    std::string_view text;
};

// One message in one locale. `id` is the stable id, never the text (i18n-v1.md section 2: "Stable
// message ids; no sentence concatenation").
struct MessageEntry {
    std::string_view id;
    std::string_view locale;
    MessagePattern pattern;
};

struct MessageCatalogConfig {
    std::span<const MessageEntry> messages;
    // The locale whose patterns are complete: a key missing everywhere else must exist here
    // ("Every catalog key must exist in the default locale", i18n-v1.md section 8.1).
    std::string_view default_locale;
    // How many missing-key diagnostics to keep. Bounded, like every diagnostic ring in the
    // framework (ADR-022): a UI that renders an unknown id every frame must not grow a log.
    std::size_t diagnostic_capacity = 16;
};

// One recorded miss. Values, not log lines: the app decides whether to surface them.
struct MessageDiagnostic {
    std::string id;
    std::string requested_locale;
    // The locale the returned text actually came from; empty when even the default locale missed
    // and the caller got the visible key back.
    std::string served_locale;
};

// Bounded ring plus counters, mirrored from the storage module's shape so one diagnostics consumer
// reads both.
struct MessageDiagnostics {
    std::size_t capacity = 0;
    std::vector<MessageDiagnostic> entries;
    std::size_t dropped = 0;
    std::uint64_t misses = 0;
    std::uint64_t default_fallbacks = 0;
    std::uint64_t visible_keys = 0;
};

// The compiled catalog: messages sorted by (id, locale), with the fallback chain resolved against a
// `LocaleTable` at lookup time rather than duplicated per key.
class MessageCatalog {
public:
    static constexpr std::size_t kMaxMessages = 8192;

    // Validates the table: a duplicate (id, locale), an unknown locale, an id whose text is missing
    // from `default_locale`, or an empty table is an error - the runtime half of the spec's
    // "unused/missing/duplicate keys fail CI".
    [[nodiscard]] static std::expected<MessageCatalog, I18nError>
    create(const MessageCatalogConfig& config, const LocaleTable& locales);

    MessageCatalog(MessageCatalog&&) noexcept = default;
    MessageCatalog& operator=(MessageCatalog&&) noexcept = default;
    MessageCatalog(const MessageCatalog&) = delete;
    MessageCatalog& operator=(const MessageCatalog&) = delete;

    // The chain for a resolved locale: exact locale first, then its language, then the table's
    // fallback, then the default. `resolved` is expected to come from `LocaleTable::resolve`, so
    // it is one of the supported tags; an unknown value simply misses every step.
    [[nodiscard]] std::string_view text(std::string_view id,
                                        std::string_view resolved_locale) const;

    // True when `id` exists in the default locale (the completeness rule above).
    [[nodiscard]] bool contains(std::string_view id) const;

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }
    [[nodiscard]] std::size_t locale_count() const noexcept {
        return locale_count_;
    }

    // Recorded misses, in insertion order up to the ring's capacity.
    [[nodiscard]] MessageDiagnostics diagnostics() const;
    void clear_diagnostics();

private:
    struct Entry {
        std::string id;
        std::string locale;
        std::string text;
    };

    // The section 8.1 chain for one lookup, built on the stack: resolved locale, its language, the
    // table fallback, the default. At most four steps, so a lookup never allocates to build it.
    struct Chain {
        std::string_view steps[4];
        std::size_t count = 0;
    };

    MessageCatalog() = default;

    [[nodiscard]] Chain chain_for(std::string_view resolved_locale) const noexcept;
    [[nodiscard]] const Entry* find(std::string_view id, std::string_view locale) const noexcept;
    void record_miss(std::string_view id, std::string_view resolved_locale,
                     std::string_view served_locale) const;

    std::vector<Entry> entries_;
    std::string fallback_locale_;
    std::string default_locale_;
    std::size_t locale_count_ = 0;
    std::size_t diagnostic_capacity_ = 0;

    // Diagnostics are mutable state behind a const lookup, so a `text()` call on a const catalog
    // can still record the miss. It is a value counter, not a cache: a lookup never allocates for a
    // hit.
    mutable std::vector<MessageDiagnostic> diagnostics_;
    mutable std::size_t dropped_ = 0;
    mutable std::uint64_t misses_ = 0;
    mutable std::uint64_t default_fallbacks_ = 0;
    mutable std::uint64_t visible_keys_ = 0;
};

} // namespace tessera::appfw
