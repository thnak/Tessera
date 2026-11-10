// SPDX-License-Identifier: MIT
//
// Spike harness for task-94 (ICU reduced static data, measured cost). NOT part of the Tessera
// build and not linked into the framework: it links the pinned ICU 76.1 DLLs only to exercise a
// candidate data blob through the public C APIs the text stack and i18n service would use.
//
// Build (Windows, ICU 76.1 binary distribution):
//   clang icu_subset_probe.c -I<icu>/include -L<icu>/lib64 -licuuc -licuin -o icu_subset_probe.exe
//   (copy icuuc76.dll, icuin76.dll, icutu76.dll next to the exe; do NOT copy icudt76.dll, so a
//    missing --data blob cannot silently fall back to the full system data)
//
// Run: icu_subset_probe.exe --data <icudt76l.dat> --locale ja_JP
//
// With EMBEDDED_DATA defined the blob is the linked `icudt76l_dat` symbol (the artifact a static
// data build ships, produced by genccode/pkgdata), and no --data argument is read.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unicode/ubrk.h>
#include <unicode/ucol.h>
#include <unicode/udat.h>
#include <unicode/udata.h>
#include <unicode/uloc.h>
#include <unicode/unorm2.h>
#include <unicode/unum.h>
#include <unicode/ures.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>
#include <unicode/uvernum.h>

#ifdef EMBEDDED_DATA
// genccode -o emits a pointer symbol to the data image (the static data packaging artifact).
extern "C" const char icudt76l_dat[];
#endif

static const UChar kLineText[] = {0x48,   0x65,   0x6C,   0x6C,   0x6F,   0x2C,
                                  0x20,   0x4E16, 0x754C, 0x21,   0x20,   0x3053,
                                  0x3093, 0x306B, 0x3061, 0x306F, 0x3002, 0};
static const UChar kWordText[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F,   0x20,   0x77, 0x6F,
                                  0x72, 0x6C, 0x64, 0x20, 0x4E16, 0x754C, 0};
static const UChar kUpperText[] = {0x69, 0};       /* "i" */
static const UChar kNfcText[] = {0x65, 0x0301, 0}; /* "e" + U+0301 */

static const char* status_name(UErrorCode code) {
    switch (code) {
    case U_ZERO_ERROR:
        return "U_ZERO_ERROR";
    case U_USING_FALLBACK_WARNING:
        return "U_USING_FALLBACK_WARNING";
    case U_USING_DEFAULT_WARNING:
        return "U_USING_DEFAULT_WARNING";
    case U_MISSING_RESOURCE_ERROR:
        return "U_MISSING_RESOURCE_ERROR";
    case U_INVALID_FORMAT_ERROR:
        return "U_INVALID_FORMAT_ERROR";
    case U_FILE_ACCESS_ERROR:
        return "U_FILE_ACCESS_ERROR";
    case U_INVALID_CHAR_FOUND:
        return "U_INVALID_CHAR_FOUND";
    case U_BUFFER_OVERFLOW_ERROR:
        return "U_BUFFER_OVERFLOW_ERROR";
    case U_ILLEGAL_ARGUMENT_ERROR:
        return "U_ILLEGAL_ARGUMENT_ERROR";
    default:
        return "other";
    }
}

static void print_status(const char* key, UErrorCode code) {
    printf("%s=%s(%d)\n", key, status_name(code), (int)code);
}

static void as_utf8(const UChar* src, int32_t len, char* dst, size_t cap) {
    UErrorCode status = U_ZERO_ERROR;
    int32_t written = 0;
    u_strToUTF8(dst, (int32_t)cap, &written, src, len, &status);
    if (U_FAILURE(status) || written < 0) {
        strcpy(dst, "<unrepresentable>");
    } else {
        dst[written] = '\0';
    }
}

static char* read_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("open_failed=%s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buffer = (char*)malloc((size_t)size);
    if (!buffer || fread(buffer, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buffer);
        printf("read_failed=%s\n", path);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buffer;
}

static void probe_line_break(const char* locale) {
    UErrorCode status = U_ZERO_ERROR;
    UBreakIterator* it = ubrk_open(UBRK_LINE, locale, kLineText, -1, &status);
    if (U_FAILURE(status)) {
        print_status("brk_line", status);
        return;
    }
    int32_t breaks = 0;
    for (int32_t p = ubrk_first(it); p != UBRK_DONE; p = ubrk_next(it)) {
        breaks++;
    }
    printf("brk_line=ok(%d) breaks=%d\n", (int)status, breaks);
    ubrk_close(it);
}

static void probe_word_break(const char* locale) {
    UErrorCode status = U_ZERO_ERROR;
    UBreakIterator* it = ubrk_open(UBRK_WORD, locale, kWordText, -1, &status);
    if (U_FAILURE(status)) {
        print_status("brk_word", status);
        return;
    }
    int32_t words = 0;
    for (int32_t p = ubrk_first(it); p != UBRK_DONE; p = ubrk_next(it)) {
        if (ubrk_getRuleStatus(it) != UBRK_WORD_NONE) {
            words++;
        }
    }
    printf("brk_word=ok(%d) segments=%d\n", (int)status, words);
    ubrk_close(it);
}

// The locale bundle is where per-locale UI data lives (calendar, date, numbers, currency).
// opening it is the "does this locale pack exist" probe.
static void probe_resources(const char* locale) {
    UErrorCode status = U_ZERO_ERROR;
    UResourceBundle* bundle = ures_open(NULL, locale, &status);
    if (U_FAILURE(status) || !bundle) {
        print_status("locale_bundle", status);
        if (bundle) {
            ures_close(bundle);
        }
        return;
    }
    char actual[128];
    UErrorCode tag_status = U_ZERO_ERROR;
    uloc_getName(ures_getLocaleByType(bundle, ULOC_ACTUAL_LOCALE, &tag_status), actual,
                 (int32_t)sizeof(actual), &tag_status);
    printf("locale_bundle=%s actual_locale=%s\n", status_name(status),
           U_SUCCESS(tag_status) ? actual : "<unknown>");
    ures_close(bundle);
}

static void probe_number(const char* locale) {
    UErrorCode status = U_ZERO_ERROR;
    UNumberFormat* fmt = unum_open(UNUM_DECIMAL, NULL, 0, locale, NULL, &status);
    if (U_FAILURE(status) || !fmt) {
        print_status("unum_decimal", status);
        return;
    }
    UChar buffer[64];
    int32_t length = unum_formatDouble(fmt, 1234567.5, buffer, 64, NULL, &status);
    if (U_FAILURE(status)) {
        print_status("unum_decimal", status);
    } else {
        char utf8[128];
        as_utf8(buffer, length, utf8, sizeof(utf8));
        char actual[128];
        UErrorCode tag_status = U_ZERO_ERROR;
        uloc_getName(unum_getLocaleByType(fmt, ULOC_ACTUAL_LOCALE, &tag_status), actual,
                     (int32_t)sizeof(actual), &tag_status);
        printf("unum_decimal=%s sample=%s actual_locale=%s\n", status_name(status), utf8, actual);
    }
    unum_close(fmt);
}

static void probe_date(const char* locale) {
    UErrorCode status = U_ZERO_ERROR;
    UDateFormat* fmt = udat_open(UDAT_NONE, UDAT_DEFAULT, locale, NULL, 0, NULL, 0, &status);
    if (U_FAILURE(status) || !fmt) {
        print_status("udat_default", status);
        return;
    }
    UChar buffer[128];
    int32_t length = udat_format(fmt, 1704067200000.0, buffer, 128, NULL, &status);
    if (U_FAILURE(status)) {
        print_status("udat_default", status);
    } else {
        char utf8[256];
        as_utf8(buffer, length, utf8, sizeof(utf8));
        printf("udat_default=%s sample=%s\n", status_name(status), utf8);
    }
    udat_close(fmt);
}

static void probe_collation(const char* locale) {
    UErrorCode status = U_ZERO_ERROR;
    UCollator* coll = ucol_open(locale, &status);
    if (U_FAILURE(status) || !coll) {
        print_status("ucol", status);
        return;
    }
    printf("ucol=ok(%d)\n", (int)status);
    ucol_close(coll);
}

static void probe_case(const char* locale) {
    UErrorCode status = U_ZERO_ERROR;
    UChar upper[16];
    int32_t length = u_strToUpper(upper, 16, kUpperText, -1, locale, &status);
    if (U_FAILURE(status)) {
        print_status("case_upper", status);
        return;
    }
    char utf8[32];
    as_utf8(upper, length, utf8, sizeof(utf8));
    printf("case_upper=%s sample=%s\n", status_name(status), utf8);
}

// Locale-independent Unicode data: these must keep working for every locale subset, because
// text shaping and line breaking do not depend on the UI locale.
static void probe_unicode_core(void) {
    UErrorCode status = U_ZERO_ERROR;
    const UNormalizer2* nfc = unorm2_getNFCInstance(&status);
    if (U_FAILURE(status) || !nfc) {
        print_status("nfc", status);
    } else {
        UChar composed[8];
        int32_t length = unorm2_normalize(nfc, kNfcText, -1, composed, 8, &status);
        char utf8[16];
        if (U_FAILURE(status)) {
            print_status("nfc", status);
        } else {
            as_utf8(composed, length, utf8, sizeof(utf8));
            printf("nfc=%s sample=%s\n", status_name(status), utf8);
        }
    }
}

static void probe_codepoint_names(void) {
    UErrorCode status = U_ZERO_ERROR;
    UChar32 c = 0x4E16; /* U+4E16 CJK ideograph, from unames.icu */
    char name[128];
    int32_t length = u_charName(c, U_UNICODE_CHAR_NAME, name, (int32_t)sizeof(name), &status);
    if (U_FAILURE(status)) {
        print_status("char_name", status);
    } else {
        printf("char_name=%s sample=%s len=%d\n", status_name(status), name, (int)length);
    }
}

int main(int argc, char** argv) {
    const char* data_path = NULL;
    const char* locale = "en";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_path = argv[++i];
        } else if (strcmp(argv[i], "--locale") == 0 && i + 1 < argc) {
            locale = argv[++i];
        }
    }

    UVersionInfo version;
    u_getVersion(version);
    printf("icu_version=%d.%d.%d.%d\n", version[0], version[1], version[2], version[3]);
    printf("requested_locale=%s\n", locale);

#ifdef EMBEDDED_DATA
    {
        UErrorCode status = U_ZERO_ERROR;
        udata_setCommonData((const void*)icudt76l_dat, &status);
        print_status("set_common_data", status == U_ZERO_ERROR ? status : status);
        if (U_FAILURE(status)) {
            return 2;
        }
        printf("data_source=embedded\n");
    }
#else
    if (data_path) {
        size_t size = 0;
        char* blob = read_file(data_path, &size);
        if (!blob) {
            return 2;
        }
        UErrorCode status = U_ZERO_ERROR;
        udata_setCommonData(blob, &status);
        print_status("set_common_data", status);
        if (U_FAILURE(status)) {
            return 2;
        }
        printf("data_source=%s data_bytes=%zu\n", data_path, size);
    }
#endif

    UErrorCode default_status = U_ZERO_ERROR;
    uloc_setDefault(locale, &default_status);
    print_status("set_default_locale", default_status);

    probe_unicode_core();
    probe_codepoint_names();
    probe_line_break(locale);
    probe_word_break(locale);
    probe_resources(locale);
    probe_number(locale);
    probe_date(locale);
    probe_collation(locale);
    probe_case(locale);
    return 0;
}
