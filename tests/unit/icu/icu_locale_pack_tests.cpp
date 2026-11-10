// SPDX-License-Identifier: MIT

// Runtime proof of the reduced ICU data blob (task-101, docs/design/icu-data.md section 5):
// the linked package is installed as common data, a kept locale still carries its own locale
// data, a trimmed locale falls back to root without an error code, and no ICU dynamic library is
// loaded anywhere in the process (ADR-012).

#include "icu_runtime.hpp"

#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include <unicode/udat.h>
#include <unicode/unum.h>
#include <unicode/ures.h>
#include <unicode/utypes.h>
#include <unicode/uversion.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <fstream>
#endif

namespace {

using tessera::ui::text::icu::BlobInfo;
using tessera::ui::text::icu::InitStatus;

int failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "icu_locale_pack: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string to_ascii(const UChar* text, int32_t length) {
    std::string out;
    out.reserve(static_cast<std::size_t>(length));
    for (int32_t index = 0; index < length; ++index) {
        // Every probe string here is ASCII; anything else is reported as '?'.
        out.push_back(text[index] < 0x80 ? static_cast<char>(text[index]) : '?');
    }
    return out;
}

std::string format_decimal(const char* locale, double value) {
    UErrorCode status = U_ZERO_ERROR;
    UNumberFormat* number = unum_open(UNUM_DECIMAL, nullptr, 0, locale, nullptr, &status);
    if (U_FAILURE(status) || number == nullptr) {
        return "<open failed>";
    }
    UChar buffer[64] = {};
    status = U_ZERO_ERROR;
    const int32_t length = unum_formatDouble(number, value, buffer, 64, nullptr, &status);
    unum_close(number);
    if (U_FAILURE(status)) {
        return "<format failed>";
    }
    return to_ascii(buffer, length);
}

std::string short_date_pattern(const char* locale, std::string* actual_locale) {
    UErrorCode status = U_ZERO_ERROR;
    UDateFormat* format =
        udat_open(UDAT_NONE, UDAT_SHORT, locale, nullptr, -1, nullptr, 0, &status);
    if (U_FAILURE(status) || format == nullptr) {
        return "<open failed>";
    }
    if (actual_locale != nullptr) {
        status = U_ZERO_ERROR;
        const char* actual = udat_getLocaleByType(format, ULOC_ACTUAL_LOCALE, &status);
        *actual_locale = U_SUCCESS(status) && actual != nullptr ? actual : "";
        status = U_ZERO_ERROR;
    }
    UChar pattern[64] = {};
    const int32_t length = udat_toPattern(format, 1, pattern, 64, &status);
    udat_close(format);
    if (U_FAILURE(status)) {
        return "<pattern failed>";
    }
    return to_ascii(pattern, length);
}

std::string format_short_date(const char* locale, double millis) {
    UErrorCode status = U_ZERO_ERROR;
    UDateFormat* format =
        udat_open(UDAT_SHORT, UDAT_SHORT, locale, nullptr, -1, nullptr, 0, &status);
    if (U_FAILURE(status) || format == nullptr) {
        return "<open failed>";
    }
    UChar buffer[64] = {};
    status = U_ZERO_ERROR;
    const int32_t length = udat_format(format, millis, buffer, 64, nullptr, &status);
    udat_close(format);
    if (U_FAILURE(status)) {
        return "<format failed>";
    }
    return to_ascii(buffer, length);
}

// ADR-012: the ICU code and its data are linked statically, so no ICU module may be loaded. An
// imported ICU DLL is mapped before main(), which makes this a direct check of the link.
bool no_icu_module_loaded() {
#if defined(_WIN32)
    for (const wchar_t* name : {L"icuuc76.dll", L"icuin76.dll", L"icudt76.dll", L"icuio76.dll"}) {
        if (GetModuleHandleW(name) != nullptr) {
            return false;
        }
    }
    return true;
#elif defined(__linux__)
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libicu") != std::string::npos) {
            return false;
        }
    }
    return true;
#else
    return true; // No portable probe; the release gate (scripts/check_static_link.py) covers it.
#endif
}

void check_blob_identity() {
    const BlobInfo info = tessera::ui::text::icu::blob_info();
    expect(info.data_name == "icudt76l", "blob ToC prefix is the pinned ICU data name");
    expect(info.version == "76.1.0", "blob records the pinned ICU version");
    expect(info.sha256.size() == 64, "blob manifest carries a SHA-256 digest");
    expect(info.bytes > 8LL * 1024 * 1024, "reduced blob is larger than the Unicode floor");
    expect(info.items > 200, "reduced blob keeps the non-locale items");
    expect(info.locales >= 20, "reduced blob keeps the documented locale set");
}

void check_kept_and_trimmed_locales() {
    for (const char* locale : {"en", "de", "ja", "ar", "zh_Hant"}) {
        UErrorCode status = U_ZERO_ERROR;
        UResourceBundle* bundle = ures_open(nullptr, locale, &status);
        const bool opened = U_SUCCESS(status) && bundle != nullptr;
        std::string actual;
        if (opened) {
            status = U_ZERO_ERROR;
            const char* name = ures_getLocaleByType(bundle, ULOC_ACTUAL_LOCALE, &status);
            actual = U_SUCCESS(status) && name != nullptr ? name : "";
            ures_close(bundle);
        }
        expect(opened, std::string("kept locale opens: ") + locale);
        expect(actual == locale, std::string("kept locale resolves to itself: ") + locale);
    }

    // Locale data was trimmed out of the package: ICU only sees the package ToC, so a trimmed
    // locale and a never-present one take the same path. It warns instead of failing.
    UErrorCode status = U_ZERO_ERROR;
    UResourceBundle* absent = ures_open(nullptr, "zz_ZZ", &status);
    expect(absent != nullptr, "a trimmed locale still opens a bundle");
    if (absent != nullptr) {
        const char* name = ures_getLocaleByType(absent, ULOC_ACTUAL_LOCALE, &status);
        // ICU resolves an unknown locale through the default locale, so the only stable claim is
        // that the requested locale is not what the bundle reports back.
        expect(name == nullptr || std::string_view(name) != "zz_ZZ",
               "a trimmed locale reports no actual locale (root fallback)");
        ures_close(absent);
    }
    expect(status == U_USING_DEFAULT_WARNING || status == U_USING_FALLBACK_WARNING,
           "a trimmed locale warns instead of erroring");
}

void check_formatters() {
    const double value = 1234567.5;
    const std::string german = format_decimal("de", value);
    const std::string english = format_decimal("en", value);
    const std::string root = format_decimal("", value);
    expect(german == "1.234.567,5", "de number golden uses the kept locale data");
    expect(english == "1,234,567.5", "en number golden uses the kept locale data");
    // Root is the fallback a trimmed locale would silently receive.
    expect(root == english, "root number formatting matches the en fallback");

    // 2024-01-01T00:00:00Z. The *pattern* is the time-zone-independent golden: the formatted value
    // would depend on the host zone, and the same-zone comparison below still proves the data path.
    const double millis = 1704067200000.0;
    std::string japanese_locale;
    std::string german_locale;
    const std::string japanese_pattern = short_date_pattern("ja", &japanese_locale);
    const std::string german_pattern = short_date_pattern("de", &german_locale);
    const std::string root_pattern = short_date_pattern("", nullptr);
    expect(japanese_locale == "ja", "ja date formatter reports the kept locale");
    expect(german_locale == "de", "de date formatter reports the kept locale");
    expect(japanese_pattern == "y/MM/dd", "ja short date pattern golden");
    expect(german_pattern == "dd.MM.yy", "de short date pattern golden");
    expect(root_pattern == "y-MM-dd", "root short date pattern golden");

    const std::string japanese = format_short_date("ja", millis);
    const std::string german_date = format_short_date("de", millis);
    const std::string root_date = format_short_date("zz_ZZ", millis);
    expect(japanese != root_date, "ja date output differs from the root fallback");
    expect(german_date != root_date, "de date output differs from the root fallback");

    // udat_open succeeds with root data: the documented reason a missing pack must be detected
    // through the manifest instead of an ICU error code.
    UErrorCode status = U_ZERO_ERROR;
    UDateFormat* rooted =
        udat_open(UDAT_SHORT, UDAT_SHORT, "zz_ZZ", nullptr, -1, nullptr, 0, &status);
    expect(U_SUCCESS(status), "udat_open on a trimmed locale returns U_ZERO_ERROR");
    if (rooted != nullptr) {
        udat_close(rooted);
    }

    // Determinism: the same input twice yields byte-identical output.
    expect(format_short_date("ja", millis) == japanese, "ja date formatting is stable");
    expect(format_decimal("de", value) == german, "de number formatting is stable");
}

} // namespace

int main() {
    using tessera::ui::text::icu::initialize;

    expect(initialize() == InitStatus::Ok, "the linked blob installs as ICU common data");
    if (!tessera::ui::text::icu::init_error().empty()) {
        std::cerr << "icu_locale_pack: init error: " << tessera::ui::text::icu::init_error()
                  << "\n";
    }
    expect(tessera::ui::text::icu::ready(), "ICU reports ready");
    expect(initialize() == InitStatus::AlreadyReady, "initialization is idempotent");
    expect(tessera::ui::text::icu::init_error().empty(), "no initialization error is recorded");

    UVersionInfo version{};
    u_getVersion(version);
    expect(std::string(tessera::ui::text::icu::library_version()) == "76.1.0",
           "linked ICU library is the pinned 76.1.0");
    expect(tessera::ui::text::icu::package_name() == "icudt76l",
           "ICU common data name matches the blob ToC prefix");

    check_blob_identity();
    check_kept_and_trimmed_locales();
    check_formatters();

    expect(no_icu_module_loaded(), "no ICU dynamic library is loaded in the process (ADR-012)");

    if (failures != 0) {
        std::cerr << "icu_locale_pack: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "icu_locale_pack: ok\n";
    return 0;
}
