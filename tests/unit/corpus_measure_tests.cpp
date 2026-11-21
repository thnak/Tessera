// SPDX-License-Identifier: MIT

// Corpus measurement golden (spike #660, ADR-066 "Evidence required before Accepted").
//
// The measurement is the evidence the ADR is accepted on, so it is pinned like any other golden:
// the committed report is compared byte for byte, and a change to what the front end accepts shows
// up as a diff in review rather than as a number someone remembers. The corpus is the fixtures
// under tests/fixtures/corpus/react, which are written in this repository (MIT) rather than
// downloaded, so the measurement is reproducible from a clean checkout with no network.
//
// Two invariants:
//   1. The report over the corpus equals tests/golden/corpus/react-front-end.txt.
//   2. The corpus holds at least 30 files, so a fixture deleted without a golden update fails
//      rather than quietly shrinking the evidence.

#include "measure.hpp"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_CORPUS_DIR
#error "TESSERA_CORPUS_DIR must be defined by the build"
#endif

#ifndef TESSERA_CORPUS_GOLDEN
#error "TESSERA_CORPUS_GOLDEN must be defined by the build"
#endif

namespace {

namespace corpus = tessera::compiler::corpus;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    out = text.str();
    return true;
}

} // namespace

int main() {
    // The label is the corpus's own name, not the absolute path, so the golden is the same bytes on
    // every machine and in every checkout.
    std::string error;
    std::vector<corpus::FileMeasurement> measurements =
        corpus::measure_directory(TESSERA_CORPUS_DIR, "react", error);
    check(error.empty(), "the corpus directory is readable");
    if (!error.empty()) {
        std::cerr << error << "\n";
        return 1;
    }

    const corpus::Report report = corpus::summarize(std::move(measurements));
    // The corpus is the evidence, so a corpus that shrank is a failure, not a smaller number.
    check(report.total() >= 30, "the corpus holds at least 30 files");

    std::string golden;
    check(read_file(TESSERA_CORPUS_GOLDEN, golden), "the golden is readable");
    const std::string actual = corpus::format_report(report);
    if (actual != golden) {
        std::cerr << "FAIL: the corpus report differs from " << TESSERA_CORPUS_GOLDEN << "\n"
                  << "--- expected ---\n"
                  << golden << "--- actual ---\n"
                  << actual;
        ++failures;
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "corpus_measure passed (" << report.total() << " files, " << report.accepted
              << " accepted by the front end)\n";
    return 0;
}
