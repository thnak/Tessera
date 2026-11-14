// SPDX-License-Identifier: MIT
//
// Byte-level guarantee for the production profile: a library built with TESSERA_ENABLE_TRACING=OFF
// must contain no trace code at all. ADR-022 decision 11 and docs/specs/diagnostics-config-v1.md
// section 5 make tracing a development feature, and `docs/design/build-and-packaging.md` records
// the option. Checking the built archive (rather than trusting the linker to drop a translation
// unit nobody references) turns "it happens to be absent" into a contract: a stray `TraceSpan` on a
// frame path that drags the recorder into a shipped binary fails here.
//
// The same scanner runs over a tracing-enabled build, so a scanner that silently matched nothing
// could not pass as green.

#include "tessera/diagnostics/trace.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Strings that only the real recorder emits, each a complete literal in `trace.cpp`: the Chrome
// phase and pid fields, the frame/tick argument prefix, and the two exported symbol names. They are
// matched as byte runs, so a marker split across two literals would never match and is not used.
constexpr std::string_view kTracerMarkers[] = {"\"ph\":\"", "\"pid\":1",
                                               "\"args\":{\"frame\":", "to_chrome_trace_json",
                                               "trace_category_name"};

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>()};
}

bool contains(const std::vector<std::uint8_t>& blob, std::string_view needle) {
    if (needle.empty() || blob.size() < needle.size()) {
        return false;
    }
    const auto* const data = reinterpret_cast<const char*>(blob.data());
    const std::size_t last = blob.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (data[i + j] != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "tracing_strip: " << message << "\n";
    }
    return condition;
}

bool expect_empty_document(std::string_view message) {
    return expect(
        tessera::diagnostics::to_chrome_trace_json(tessera::diagnostics::TraceRecorder{}) == "[]",
        message);
}

} // namespace

int main(int argc, char** argv) {
    // argv[1] is the tessera_diagnostics archive the build just produced.
    if (argc < 2) {
        std::cerr << "tracing_strip: expected the diagnostics archive path\n";
        return 2;
    }
    const fs::path archive = argv[1];
    if (!expect(fs::is_regular_file(archive), "the diagnostics archive exists")) {
        std::cerr << "tracing_strip: no archive at " << archive.string() << "\n";
        return 2;
    }
    const std::vector<std::uint8_t> blob = read_file(archive);
    if (!expect(!blob.empty(), "the diagnostics archive is not empty")) {
        return 2;
    }

#if defined(TESSERA_ENABLE_TRACING)
    // Tracing on: the recorder is compiled, so its markers must be visible to the scanner.
    for (const std::string_view marker : kTracerMarkers) {
        if (!expect(contains(blob, marker), "the tracing-on archive carries every tracer marker")) {
            std::cerr << "tracing_strip: missing marker " << marker << "\n";
            return 1;
        }
    }
    tessera::diagnostics::TraceRecorder recorder(8);
    recorder.instant("span", tessera::diagnostics::TraceCategory::Frame);
    const std::string document = tessera::diagnostics::to_chrome_trace_json(recorder);
    return expect(recorder.size() == 1, "the enabled recorder keeps its events") &&
                   expect(!document.empty(), "the enabled recorder answers a document")
               ? 0
               : 1;
#else
    // Tracing off: no marker and no recorder implementation may reach the archive.
    for (const std::string_view marker : kTracerMarkers) {
        if (!expect(!contains(blob, marker), "a tracing-off archive carries no tracer marker")) {
            std::cerr << "tracing_strip: found marker " << marker << "\n";
            return 1;
        }
    }
    // The disabled recorder is a no-op that still answers the documented empty document
    // (docs/specs/headless-automation-v1.md section 7.1).
    tessera::diagnostics::TraceRecorder recorder(8);
    recorder.instant("span", tessera::diagnostics::TraceCategory::Frame);
    if (!expect(recorder.size() == 0, "the disabled recorder discards events") ||
        !expect_empty_document("the disabled recorder still answers the empty document")) {
        return 1;
    }
    std::cout << "tracing_strip: ok (tracing compiled out)\n";
    return 0;
#endif
}
