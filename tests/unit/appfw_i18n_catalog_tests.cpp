// SPDX-License-Identifier: MIT
//
// The compiled message catalog (docs/specs/i18n-v1.md sections 2 and 8.1): stable ids, the fallback
// chain across locales, the missing-key contract ("visible key/diagnostic in dev; never crash"),
// and the build-time validation rules the runtime half can still check.

#include "appfw_i18n_test_support.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace i18n_test {
namespace {

using tessera::appfw::I18nErrorCode;

void an_exact_locale_wins_and_a_miss_falls_back_in_order() {
    const LocaleTable table = make_table();
    MessageCatalog catalog = make_catalog(table);

    // Exact bundle.
    expect(catalog.text("orders.title", "de") == "Bestellungen", "de serves its own pattern");
    expect(catalog.text("orders.title", "en") == "Orders", "en serves its own pattern");
    expect(catalog.text("orders.title", "ar") == "Talabat", "ar serves its own pattern");

    // `de_AT` has its own `orders.title`, so it is exact; `orders.empty` is absent there, so the
    // chain's language step (`de`) serves it. Both are one call, and neither records a miss for the
    // exact case.
    catalog.clear_diagnostics();
    expect(catalog.text("orders.title", "de_AT") == "Bestellung", "de_AT serves its own pattern");
    expect(catalog.diagnostics().misses == 0, "an exact hit records no miss");
    expect(catalog.text("orders.empty", "de_AT") == "Keine Bestellungen",
           "a missing regional pattern falls to the language step");
    expect(catalog.diagnostics().misses == 1, "the language-step hit records one miss");
    expect(catalog.diagnostics().entries.back().served_locale == "de",
           "the diagnostic names the locale that served the pattern");

    // The full chain reaches the default: `orders.empty` exists in `en` and `de`, and a request for
    // a locale with no pattern at all lands on the default. `en` is both the default and the
    // fallback here, so the step that serves it is the fallback step (section 8.1's order).
    expect(catalog.text("orders.empty", "zh_Hant") == "No orders",
           "the chain reaches the default locale");
    expect(catalog.diagnostics().default_fallbacks >= 1, "a default-locale fallback is counted");
}

void a_missing_key_is_visible_text_and_a_bounded_diagnostic() {
    const LocaleTable table = make_table();
    MessageCatalog catalog = make_catalog(table, "en", 4);

    // "Missing key -> default locale + visible key/diagnostic in dev; never crash."
    const std::string_view text = catalog.text("orders.unknown", "de");
    expect(text == "orders.unknown", "a missing key renders as the visible key");
    expect(!text.empty(), "a missing key is never empty text");

    const auto diagnostics = catalog.diagnostics();
    expect(diagnostics.visible_keys == 1, "the visible key is counted");
    expect(diagnostics.entries.size() == 1, "the miss is recorded once");
    expect(diagnostics.entries.front().id == "orders.unknown", "the diagnostic names the id");
    expect(diagnostics.entries.front().requested_locale == "de",
           "the diagnostic names the requested locale");
    expect(diagnostics.entries.front().served_locale.empty(),
           "nothing served the key, so no served locale is recorded");

    // The ring is bounded: a UI that renders an unknown id every frame must not grow a log.
    for (int index = 0; index < 10; ++index) {
        (void)catalog.text("orders.unknown", "de");
    }
    const auto bounded = catalog.diagnostics();
    expect(bounded.capacity == 4, "the ring keeps its configured capacity");
    expect(bounded.entries.size() == 4, "the ring does not grow past its capacity");
    expect(bounded.dropped == 7, "every evicted diagnostic is counted");
    expect(bounded.misses == 11, "every miss is counted, including evicted ones");

    // The ring is cleared wholesale, which is what a caller does after logging one frame's misses.
    catalog.clear_diagnostics();
    expect(catalog.diagnostics().entries.empty(), "clear_diagnostics empties the ring");
    expect(catalog.diagnostics().misses == 0, "clear_diagnostics resets the counters");
}

void a_capacity_of_zero_keeps_the_counters_but_not_the_ring() {
    const LocaleTable table = make_table();
    const MessageCatalog catalog = make_catalog(table, "en", 0);
    expect(catalog.text("orders.missing", "en") == "orders.missing", "the key is still visible");
    const auto diagnostics = catalog.diagnostics();
    expect(diagnostics.entries.empty(), "a zero-capacity ring records no entry");
    expect(diagnostics.misses == 1, "a zero-capacity ring still counts the miss");
    expect(diagnostics.visible_keys == 1, "a zero-capacity ring still counts the visible key");
}
void every_id_must_exist_in_the_default_locale() {
    const LocaleTable table = make_table();

    // An id that only exists in a non-default locale is exactly the mistake section 8.1 forbids:
    // the default locale is the chain's last step, so such an id can never be served in full.
    const MessageEntry partial[] = {
        {"orders.title", "en", {"Orders"}},
        {"orders.title", "de", {"Bestellungen"}},
        {"orders.only_german", "de", {"Nur deutsch"}},
    };
    MessageCatalogConfig config;
    config.messages = partial;
    config.default_locale = "en";
    const auto result = MessageCatalog::create(config, table);
    expect(!result.has_value(), "an id missing from the default locale is rejected");
    expect(result.error().code == I18nErrorCode::MissingDefaultMessage,
           "the gap reports MissingDefaultMessage");
    expect(result.error().subject == "orders.only_german", "the error names the missing id");
}

void duplicate_and_unknown_entries_are_rejected() {
    const LocaleTable table = make_table();

    // The same (id, locale) twice: a generated table cannot be allowed to be ambiguous.
    const MessageEntry duplicated[] = {
        {"orders.title", "en", {"Orders"}},
        {"orders.title", "en", {"Orders!"}},
    };
    MessageCatalogConfig config;
    config.messages = duplicated;
    config.default_locale = "en";
    const auto duplicate = MessageCatalog::create(config, table);
    expect(!duplicate.has_value(), "a duplicate (id, locale) is rejected");
    expect(duplicate.error().code == I18nErrorCode::DuplicateMessageId,
           "the duplicate reports DuplicateMessageId");

    // A locale the table does not support: the pattern could never be selected.
    const MessageEntry unknown_locale[] = {
        {"orders.title", "en", {"Orders"}},
        {"orders.title", "fr", {"Commandes"}},
    };
    config.messages = unknown_locale;
    const auto unknown = MessageCatalog::create(config, table);
    expect(!unknown.has_value(), "an unsupported catalog locale is rejected");
    expect(unknown.error().code == I18nErrorCode::UnsupportedLocale,
           "it reports UnsupportedLocale rather than blaming the default");
    expect(unknown.error().subject == "fr", "the error names the unsupported locale");
}

void an_empty_catalog_is_rejected() {
    const LocaleTable table = make_table();
    MessageCatalogConfig config;
    config.default_locale = "en";
    const auto result = MessageCatalog::create(config, table);
    expect(!result.has_value(), "an empty catalog is rejected");
    expect(result.error().code == I18nErrorCode::EmptyCatalog, "it reports EmptyCatalog");
}

void the_catalog_reports_what_it_holds() {
    const LocaleTable table = make_table();
    const MessageCatalog catalog = make_catalog(table);
    expect(catalog.size() == all_messages().size(), "every compiled entry is retained");
    expect(catalog.locale_count() == 5, "the catalog counts its distinct locales");
    expect(catalog.contains("orders.title"), "contains() finds an id in the default locale");
    expect(!catalog.contains("orders.unknown"), "contains() is false for an unknown id");

    // A lookup in an unsupported locale still terminates on the fallback/default instead of
    // returning nothing: the same rule as an unsupported locale request.
    expect(catalog.text("orders.title", "zz") == "Orders",
           "an unknown locale resolves through the fallback chain");
}

void lookup_is_deterministic_across_catalogs() {
    // Two independently built catalogs over the same data agree on every string: the table is
    // sorted and the chain is fixed, so insertion order cannot leak into the result.
    const LocaleTable table = make_table();
    const MessageCatalog first = make_catalog(table);
    const MessageCatalog second = make_catalog(table);
    const char* locales[] = {"en", "de", "de_AT", "ar", "zh", "zh_Hant", "zz"};
    for (const char* locale : locales) {
        expect(first.text("orders.title", locale) == second.text("orders.title", locale),
               "two catalogs agree for every locale");
    }
}

} // namespace

int run_catalog_tests() {
    const int before = g_failures;
    an_exact_locale_wins_and_a_miss_falls_back_in_order();
    a_missing_key_is_visible_text_and_a_bounded_diagnostic();
    a_capacity_of_zero_keeps_the_counters_but_not_the_ring();
    every_id_must_exist_in_the_default_locale();
    duplicate_and_unknown_entries_are_rejected();
    an_empty_catalog_is_rejected();
    the_catalog_reports_what_it_holds();
    lookup_is_deterministic_across_catalogs();
    return g_failures - before;
}

} // namespace i18n_test
