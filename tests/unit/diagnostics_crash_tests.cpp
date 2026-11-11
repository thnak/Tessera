// SPDX-License-Identifier: MIT

// Crash-report metadata: canonical JSON, path redaction, grouping signature and the bounded caps
// (docs/specs/crash-reporting-v1.md sections 4-6.1). Deterministic: no clock, no addresses.

#include "tessera/diagnostics/crash.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::diagnostics::crash_metadata_json;
using tessera::diagnostics::crash_signature;
using tessera::diagnostics::CrashBreadcrumb;
using tessera::diagnostics::CrashFrame;
using tessera::diagnostics::CrashMetadata;
using tessera::diagnostics::CrashReportLimits;
using tessera::diagnostics::redact_path;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(const std::string& actual, const std::string& expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

void test_redact_path() {
    check_eq(redact_path("C:\\Users\\someone\\app\\main.cpp"), "main.cpp", "windows path");
    check_eq(redact_path("/home/someone/app/main.cpp"), "main.cpp", "posix path");
    check_eq(redact_path("main.cpp"), "main.cpp", "bare file name is unchanged");
    check_eq(redact_path(""), "", "empty stays empty");
    check_eq(redact_path("dir/"), "dir/", "a trailing separator keeps the input");
}

void test_signature() {
    const std::array<CrashFrame, 3> frames{
        {CrashFrame{"C:\\app\\app.exe", "tessera::app::run", 31, 0},
         CrashFrame{"app.exe", "main", 7, 0}, CrashFrame{"app.exe", "", 9, 0}}};
    CrashMetadata metadata;
    metadata.build_id = "bld-9";
    metadata.frames = frames;
    check_eq(crash_signature(metadata),
             "bld-9|app.exe!tessera::app::run+31|app.exe!main+7|app.exe!?+9",
             "signature names the innermost frames and redacts module paths");

    CrashReportLimits shallow;
    shallow.max_signature_depth = 1;
    check_eq(crash_signature(metadata, shallow), "bld-9|app.exe!tessera::app::run+31",
             "the signature depth cap is honored");
}

void test_full_document() {
    const std::array<CrashFrame, 1> frames{
        {CrashFrame{"C:\\app\\app.exe", "tessera::app::run", 31, 42}}};
    const std::array<CrashBreadcrumb, 1> breadcrumbs{
        {CrashBreadcrumb{"input", "click @ Button#12", 7}}};
    CrashMetadata metadata;
    metadata.app_version = "1.2.3";
    metadata.commit = "abc123";
    metadata.build_id = "bld-9";
    metadata.platform = "windows";
    metadata.arch = "x64";
    metadata.os = "10.0";
    metadata.crash_reason = "access_violation";
    metadata.thread = "main";
    metadata.frames = frames;
    metadata.breadcrumbs = breadcrumbs;

    const std::string expected =
        R"({"schema":"tessera.crash.v1","appVersion":"1.2.3","commit":"abc123","buildId":"bld-9",)"
        R"("platform":"windows","arch":"x64","os":"10.0","reason":"access_violation","thread":"main",)"
        R"("frames":[{"module":"app.exe","symbol":"tessera::app::run","offset":31,"line":42}],)"
        R"("breadcrumbs":[{"sequence":7,"category":"input","message":"click @ Button#12"}],)"
        R"("signature":"bld-9|app.exe!tessera::app::run+31",)"
        R"("truncated":{"frames":0,"breadcrumbs":0,"text":false}})";
    check_eq(crash_metadata_json(metadata), expected, "the canonical document is byte-stable");
}

void test_empty_document() {
    const std::string document = crash_metadata_json(CrashMetadata{});
    check_eq(document,
             R"({"schema":"tessera.crash.v1","frames":[],"breadcrumbs":[],"signature":"",)"
             R"("truncated":{"frames":0,"breadcrumbs":0,"text":false}})",
             "an empty report is still a valid object");
}

void test_frame_cap_keeps_innermost() {
    std::array<CrashFrame, 5> frames{};
    for (std::size_t index = 0; index < frames.size(); ++index) {
        frames[index] = CrashFrame{"app.exe", "f", index, 0};
    }
    CrashMetadata metadata;
    metadata.frames = frames;
    CrashReportLimits limits;
    limits.max_frames = 2;
    const std::string document = crash_metadata_json(metadata, limits);
    check(document.find("\"offset\":0") != std::string::npos, "the innermost frame is kept");
    check(document.find("\"offset\":1") != std::string::npos, "the second frame is kept");
    check(document.find("\"offset\":2") == std::string::npos, "older frames are dropped");
    check(document.find(R"("truncated":{"frames":3,"breadcrumbs":0,"text":false})") !=
              std::string::npos,
          "the dropped frame count is reported");
}

void test_breadcrumb_cap_keeps_newest() {
    std::array<CrashBreadcrumb, 4> breadcrumbs{};
    for (std::size_t index = 0; index < breadcrumbs.size(); ++index) {
        breadcrumbs[index] = CrashBreadcrumb{"input", "step", index};
    }
    CrashMetadata metadata;
    metadata.breadcrumbs = breadcrumbs;
    CrashReportLimits limits;
    limits.max_breadcrumbs = 2;
    const std::string document = crash_metadata_json(metadata, limits);
    check(document.find("\"sequence\":2") != std::string::npos, "the newest breadcrumbs are kept");
    check(document.find("\"sequence\":3") != std::string::npos, "the last breadcrumb is kept");
    check(document.find("\"sequence\":0") == std::string::npos, "the oldest breadcrumb is dropped");
    check(document.find("\"breadcrumbs\":2") != std::string::npos,
          "the dropped breadcrumb count is reported");
}

void test_escaping_and_truncation() {
    std::string control = "a\\b\nc";
    control.push_back('\x01'); // split so the hex escape cannot swallow the next character
    control.push_back('d');
    const std::array<CrashBreadcrumb, 1> breadcrumbs{{CrashBreadcrumb{"in\"put", control, 1}}};
    CrashMetadata metadata;
    metadata.breadcrumbs = breadcrumbs;
    const std::string document = crash_metadata_json(metadata);
    check(document.find(R"("category":"in\"put")") != std::string::npos, "quotes are escaped");
    check(document.find(R"("message":"a\\b\nc\u0001d")") != std::string::npos,
          "backslash, newline and control bytes are escaped");

    // A multi-byte character must not be split by the byte cap: the document stays valid UTF-8.
    const std::array<CrashBreadcrumb, 1> wide{{{CrashBreadcrumb{"ui", "aaaaaaaaa\xC3\xA9", 2}}}};
    CrashMetadata capped;
    capped.breadcrumbs = wide;
    CrashReportLimits limits;
    limits.max_breadcrumb_bytes = 10; // would cut the two bytes of U+00E9
    const std::string cut = crash_metadata_json(capped, limits);
    check(cut.find("aaaaaaaaa\"") != std::string::npos,
          "a truncated multi-byte character is dropped whole");
}

void test_text_cap_stops_cleanly() {
    std::array<CrashFrame, 8> frames{};
    for (std::size_t index = 0; index < frames.size(); ++index) {
        frames[index] = CrashFrame{"app.exe", "a_rather_long_symbol_name_for_the_cap", index, 0};
    }
    CrashMetadata metadata;
    metadata.frames = frames;
    CrashReportLimits limits;
    limits.max_text_bytes = 260;
    const std::string document = crash_metadata_json(metadata, limits);
    check(document.back() == '}', "a capped document still closes");
    check(document.find("\"text\":true") != std::string::npos, "the text cap is reported");
    // The arrays respect the budget; the fixed envelope adds at most the documented 1 KiB reserve.
    check(document.size() <= limits.max_text_bytes + 1024, "the document respects the text budget");
}

} // namespace

int main() {
    test_redact_path();
    test_signature();
    test_full_document();
    test_empty_document();
    test_frame_cap_keeps_innermost();
    test_breadcrumb_cap_keeps_newest();
    test_escaping_and_truncation();
    test_text_cap_stops_cleanly();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
