// SPDX-License-Identifier: MIT

// LocaleResolver against the linked reduced-data manifest (task-101,
// docs/design/icu-data.md section 5): a kept locale resolves exactly, a trimmed region falls back
// to its kept ancestor with a diagnostic, and an unknown locale falls back to the default instead
// of silently formatting root data. Also pins the direction used by shaping.

#include "locale_resolver.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::text::LocaleResolution;
using tessera::ui::text::LocaleResolver;
using tessera::ui::text::TextDirection;

int failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "locale_resolver: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool expect_resolution(const LocaleResolution& resolution, std::string_view resolved,
                       bool right_to_left, std::string_view message) {
    const bool ok = resolution.resolved == resolved && resolution.right_to_left == right_to_left;
    if (!ok) {
        std::cerr << "locale_resolver: " << message << ": requested='" << resolution.requested
                  << "' resolved='" << resolution.resolved << "' rtl=" << resolution.right_to_left
                  << " (expected '" << resolved << "' rtl=" << right_to_left << ")\n";
        ++failures;
    }
    return ok;
}

void check_manifest() {
    expect(LocaleResolver::kept_locale_count() >= 20, "the manifest carries the documented set");
    expect(LocaleResolver::is_kept("en"), "en is kept");
    expect(LocaleResolver::is_kept("ja"), "ja is kept");
    expect(LocaleResolver::is_kept("zh_Hant"), "zh_Hant is kept");
    expect(!LocaleResolver::is_kept("ja_JP"), "ja_JP is not its own bundle");
    expect(!LocaleResolver::is_kept("zz"), "zz is not kept");
    // The manifest is sorted, which keeps the generated artifacts byte-stable.
    for (std::size_t index = 1; index < LocaleResolver::kept_locale_count(); ++index) {
        if (LocaleResolver::kept_locale(index - 1) >= LocaleResolver::kept_locale(index)) {
            expect(false, "manifest locale ids are sorted and unique");
            break;
        }
    }
}

void check_exact_and_fallback() {
    const LocaleResolution exact = LocaleResolver::resolve("en");
    expect(exact.kept, "an exact request is marked kept");
    expect(exact.diagnostic.empty(), "an exact request has no diagnostic");
    expect(exact.fallback.empty(), "an exact request has no fallback");
    expect_resolution(exact, "en", false, "en is left-to-right");

    // A trimmed region falls back to its kept language bundle and says so.
    const LocaleResolution region = LocaleResolver::resolve("de_AT");
    expect(!region.kept, "de_AT is not kept");
    expect(region.fallback == "de", "de_AT falls back to de");
    expect(region.diagnostic == "icu.locale.fallback requested=de_AT resolved=de",
           "a fallback emits the structured diagnostic");
    expect_resolution(region, "de", false, "de_AT resolves to de");

    // An unknown locale falls back to the default with a missing diagnostic.
    const LocaleResolution unknown = LocaleResolver::resolve("zz_ZZ");
    expect(!unknown.kept, "zz_ZZ is not kept");
    expect(unknown.resolved == "en", "zz_ZZ resolves to the default locale");
    expect(unknown.diagnostic == "icu.locale.missing requested=zz_ZZ",
           "a missing locale emits the structured diagnostic");

    // Normalization: BCP-47 case/separators and a script subtag.
    expect(LocaleResolver::resolve("pt-BR").resolved == "pt_BR", "BCP-47 separators normalize");
    expect(LocaleResolver::resolve("PT_br").resolved == "pt_BR", "case normalizes");
    expect(LocaleResolver::resolve("zh-hant").resolved == "zh_Hant", "script subtags normalize");
    expect(LocaleResolver::fallback_for("de_DE_CH") == "de", "the fallback chain truncates");
    expect(LocaleResolver::fallback_for("en_AU") == "en", "an unlisted region falls back to en");
    expect(LocaleResolver::fallback_for("en_GB").empty(), "a listed region has no fallback");
    expect(LocaleResolver::is_kept("en_GB"), "en_GB is kept as its own bundle");
}

void check_direction() {
    expect(LocaleResolver::resolve("ar").right_to_left, "ar is right-to-left");
    expect(LocaleResolver::resolve("he").right_to_left, "he is right-to-left");
    expect(!LocaleResolver::resolve("tr").right_to_left, "tr is left-to-right");
    expect(!LocaleResolver::resolve("zh_Hant").right_to_left, "zh_Hant is left-to-right");
    // A trimmed RTL region still reports the ancestor's direction.
    expect(LocaleResolver::resolve("ar_EG").right_to_left, "ar_EG inherits ar's direction");
}

void check_determinism() {
    const LocaleResolution first = LocaleResolver::resolve("de_AT");
    const LocaleResolution second = LocaleResolver::resolve("de_AT");
    expect(first.resolved == second.resolved && first.diagnostic == second.diagnostic &&
               first.fallback == second.fallback,
           "resolution is deterministic");
}

} // namespace

int main() {
    check_manifest();
    check_exact_and_fallback();
    check_direction();
    check_determinism();

    if (failures != 0) {
        std::cerr << "locale_resolver: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "locale_resolver: ok\n";
    return 0;
}
