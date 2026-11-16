// SPDX-License-Identifier: MIT
//
// Locale negotiation (docs/specs/i18n-v1.md sections 1 and 8.1). Every case here is a pure function
// of (table, request list): no machine state is consulted, so the expected value is the spec's
// chain written out, not a sample of what some OS happens to return.

#include "appfw_i18n_test_support.hpp"

#include <string>
#include <string_view>

namespace i18n_test {
namespace {

using tessera::appfw::I18nErrorCode;
using tessera::appfw::normalize_locale_tag;

void normalization_is_the_reduced_canonical_form() {
    // The rule `src/ui/text/locale_resolver.cpp` already documents: separators unify, the language
    // lowercases, a two-letter region uppercases, a four-letter script title-cases.
    expect(normalize_locale_tag("pt-BR") == "pt_BR", "pt-BR normalizes to pt_BR");
    expect(normalize_locale_tag("PT_br") == "pt_BR", "PT_br normalizes to pt_BR");
    expect(normalize_locale_tag("zh-hant") == "zh_Hant", "zh-hant normalizes to zh_Hant");
    expect(normalize_locale_tag("sr-Latn-RS") == "sr_Latn_RS",
           "a script and a region are cased separately and the order is kept");
    // An unparseable tag is empty, which the resolver treats as "no request" rather than guessing.
    expect(normalize_locale_tag("").empty(), "an empty tag normalizes to empty");
    expect(normalize_locale_tag("-_").empty(), "a separator-only tag normalizes to empty");
    expect(normalize_locale_tag("123").empty(), "a numeric first subtag is not a language");
    expect(normalize_locale_tag("en__US").empty(), "an empty subtag rejects the whole tag");
}

void the_chain_walks_exact_then_language_script_then_language() {
    const LocaleTable table = make_table();

    const LocaleResolution exact = table.resolve("de_AT");
    expect(exact.match == LocaleMatch::Exact, "de_AT is an exact match");
    expect(exact.resolved == "de_AT", "de_AT resolves to itself");
    expect(exact.exact(), "exact() is true for the exact step");

    // `de_CH` is not supported: language+script does not apply (no script subtag), so the language
    // step finds `de`. This is the "regional variants inherit base language" rule.
    const LocaleResolution language = table.resolve("de_CH");
    expect(language.match == LocaleMatch::Language, "de_CH falls to the language step");
    expect(language.resolved == "de", "de_CH resolves to de");
    expect(!language.exact(), "a language-step resolution is not exact");
    expect(language.within_requested_language(), "the language step stays inside the request");

    // `zh_Hant_TW`: exact misses, language+script finds `zh_Hant`, and it stops there rather than
    // falling through to `zh` - the script is the part that decides which bundle serves the text.
    const LocaleResolution script = table.resolve("zh_Hant_TW");
    expect(script.match == LocaleMatch::LanguageScript, "zh_Hant_TW falls to language+script");
    expect(script.resolved == "zh_Hant", "zh_Hant_TW resolves to zh_Hant");
    expect(script.within_requested_language(), "language+script stays inside the request");

    // ... and a language with no exact bundle at all stops at the language step.
    const LocaleResolution plain = table.resolve("zh_CN");
    expect(plain.match == LocaleMatch::Language, "zh_CN falls to the language step");
    expect(plain.resolved == "zh", "zh_CN resolves to zh");
}

void a_total_miss_lands_on_the_fallback_then_the_default() {
    const LocaleTable table = make_table("en", "en");

    const LocaleResolution missed = table.resolve("zz_ZZ");
    expect(missed.match == LocaleMatch::Fallback, "an unsupported locale uses the fallback");
    expect(missed.resolved == "en", "the fallback locale is served");
    expect(!missed.resolved.empty(), "an unsupported locale never resolves to empty text");
    expect(missed.requested == "zz_ZZ", "the request is reported for the diagnostic");

    // The chain's last step: with no fallback entry the default is the terminal step, and it is
    // reported as `Default` rather than `Fallback` so a caller can tell the two apart.
    const LocaleTable no_fallback = make_table("", "de");
    const LocaleResolution defaulted = no_fallback.resolve("zz");
    expect(defaulted.match == LocaleMatch::Default,
           "without a fallback the default is the last step");
    expect(defaulted.resolved == "de", "the default locale is served");

    // A request that normalizes to nothing is "no request", not "match everything".
    const LocaleResolution empty = table.resolve("");
    expect(empty.match == LocaleMatch::Fallback, "an empty request takes the fallback step");
    expect(empty.resolved == "en", "an empty request resolves to a supported locale");
}

void the_request_list_is_priority_ordered() {
    const LocaleTable table = make_table();

    // The spec's four inputs (section 1) arrive as one ordered list: a user override first, then
    // the app default, then the OS locales. The first request that reaches a supported locale wins,
    // and `requested` names it rather than the earlier request that was skipped.
    const std::string_view requests[] = {"zz", "de", "en"};
    const LocaleResolution resolution = table.resolve(requests);
    expect(resolution.resolved == "de", "the first request in the list that resolves wins");
    expect(resolution.requested == "de",
           "requested names the request that served the locale, not the first one tried");
    expect(resolution.match == LocaleMatch::Exact, "that request was served exactly");

    const std::string_view supported_first[] = {"de_CH", "en"};
    const LocaleResolution first = table.resolve(supported_first);
    expect(first.resolved == "de", "the first request that resolves wins");

    // An empty list is not an error: it lands on the same total-miss terminal step as an
    // unsupported request, because a locale table always has a default.
    const LocaleResolution none = table.resolve(std::span<const std::string_view>{});
    expect(none.resolved == "en", "an empty request list resolves to the default");
}

void direction_comes_from_the_table_not_from_the_machine() {
    const LocaleTable table = make_table();
    expect(table.resolve("ar").direction == TextDirection::RightToLeft, "ar is right-to-left");
    expect(table.resolve("ar_EG").direction == TextDirection::RightToLeft,
           "ar_EG inherits ar's direction through the language step");
    expect(table.resolve("en").direction == TextDirection::LeftToRight, "en is left-to-right");
    // An unsupported request that falls back to `en` is LTR: the direction belongs to the locale
    // that is actually served, not to the request.
    expect(table.resolve("zz").direction == TextDirection::LeftToRight,
           "a fallback takes the fallback locale's direction");
}

void a_bad_table_is_a_configuration_error() {
    // Empty table.
    {
        LocaleTableConfig config;
        config.default_locale = "en";
        const auto result = LocaleTable::create(config);
        expect(!result.has_value(), "an empty locale table is rejected");
        expect(result.error().code == I18nErrorCode::EmptyLocaleTable,
               "the empty table reports EmptyLocaleTable");
    }
    // Duplicate tag, including two spellings of the same tag.
    {
        LocaleTableConfig config = table_config();
        const SupportedLocale duplicated[] = {{"en", TextDirection::LeftToRight},
                                              {"EN", TextDirection::LeftToRight}};
        config.locales = duplicated;
        const auto result = LocaleTable::create(config);
        expect(!result.has_value(), "a duplicate tag is rejected");
        expect(result.error().code == I18nErrorCode::DuplicateLocale,
               "the duplicate reports DuplicateLocale");
        expect(result.error().subject == "en", "the error names the canonical tag");
    }
    // A default locale nobody supports would make every total miss resolve to nothing.
    {
        const auto result = LocaleTable::create(table_config("en", "fr"));
        expect(!result.has_value(), "an unsupported default locale is rejected");
        expect(result.error().code == I18nErrorCode::UnknownDefaultLocale,
               "the bad default reports UnknownDefaultLocale");
    }
    // The fallback step has the same requirement.
    {
        const auto result = LocaleTable::create(table_config("fr", "en"));
        expect(!result.has_value(), "an unsupported fallback locale is rejected");
        expect(result.error().code == I18nErrorCode::UnknownFallbackLocale,
               "the bad fallback reports UnknownFallbackLocale");
    }
    // An unparseable tag is rejected rather than silently skipped.
    {
        LocaleTableConfig config = table_config();
        const SupportedLocale bad[] = {{"", TextDirection::LeftToRight}};
        config.locales = bad;
        const auto result = LocaleTable::create(config);
        expect(!result.has_value(), "an unparseable tag is rejected");
    }
}

void the_table_reports_what_it_holds() {
    const LocaleTable table = make_table();
    expect(table.size() == 6, "the table holds every supported locale");
    expect(table.default_locale() == "en", "the default locale is normalized and reported");
    expect(table.fallback_locale() == "en", "the fallback locale is normalized and reported");
    expect(table.contains("DE-at"), "contains() normalizes its argument");
    expect(!table.contains("fr"), "an unsupported tag is not contained");

    // Sorted, so an enumeration (a settings picker) is deterministic.
    std::string previous;
    for (std::size_t index = 0; index < table.size(); ++index) {
        const SupportedLocale locale = table.at(index);
        expect(!locale.tag.empty(), "at() returns a tag for every index");
        expect(previous.empty() || previous < std::string(locale.tag),
               "at() enumerates the table in sorted tag order");
        previous = locale.tag;
    }
    expect(table.at(table.size()).tag.empty(), "at() past the end is empty");
}

} // namespace

int run_locale_tests() {
    const int before = g_failures;
    normalization_is_the_reduced_canonical_form();
    the_chain_walks_exact_then_language_script_then_language();
    a_total_miss_lands_on_the_fallback_then_the_default();
    the_request_list_is_priority_ordered();
    direction_comes_from_the_table_not_from_the_machine();
    a_bad_table_is_a_configuration_error();
    the_table_reports_what_it_holds();
    return g_failures - before;
}

} // namespace i18n_test
