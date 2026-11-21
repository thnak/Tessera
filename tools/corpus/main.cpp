// SPDX-License-Identifier: MIT

// `tessera_corpus_measure`: run the JSX/TSX front end over one or more corpus directories and
// report, per rejected form, the diagnostic code and its frequency (spike #660, ADR-066).
//
// Exit codes: 0 the report was written; 1 a corpus file could not be read; 2 usage.
//
// A corpus root is a directory of `.tsx` files, walked recursively in sorted path order so the
// report is a pure function of the tree. A file is named `<root as given>/<relative path>`, so two
// roots cannot collide and the report does not depend on where the checkout lives. Nothing here
// reads the clock, the environment or the network.

#include "measure.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace corpus = tessera::compiler::corpus;

constexpr std::string_view kUsage =
    "usage: tessera_corpus_measure [--out FILE] [--label NAME] [--verbose] DIR...\n"
    "\n"
    "  DIR...     directories of .tsx corpus files, each walked recursively\n"
    "  --out FILE write the report here instead of standard output\n"
    "  --label NAME\n"
    "             name the files `<NAME>/<relative path>` instead of `<DIR>/<relative path>`;\n"
    "             the golden test uses this so the report does not name the checkout\n"
    "  --verbose  also list every file with its component count\n";

// A root as the report spells it: no trailing separator, `/` separators, so `./a/` and `a` agree.
std::string root_label(std::string_view root) {
    std::string label(root);
    while (!label.empty() && (label.back() == '/' || label.back() == '\\')) {
        label.pop_back();
    }
    std::replace(label.begin(), label.end(), '\\', '/');
    return label;
}

} // namespace

int main(int argc, char** argv) {
    std::string out_path;
    std::string label;
    std::vector<std::string> roots;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::fputs(kUsage.data(), stdout);
            return 0;
        }
        if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--out") {
            if (i + 1 >= argc) {
                std::fputs("tessera_corpus_measure: `--out` needs a value\n", stderr);
                return 2;
            }
            out_path = argv[++i];
        } else if (arg == "--label") {
            if (i + 1 >= argc) {
                std::fputs("tessera_corpus_measure: `--label` needs a value\n", stderr);
                return 2;
            }
            label = argv[++i];
        } else if (!arg.empty() && arg.front() == '-') {
            std::fprintf(stderr, "tessera_corpus_measure: unknown option %s\n", argv[i]);
            return 2;
        } else {
            roots.emplace_back(arg);
        }
    }
    if (roots.empty()) {
        std::fputs(kUsage.data(), stderr);
        return 2;
    }
    if (!label.empty() && roots.size() != 1) {
        std::fputs("tessera_corpus_measure: `--label` names one root\n", stderr);
        return 2;
    }

    std::vector<corpus::FileMeasurement> measurements;
    for (const std::string& root : roots) {
        std::string error;
        std::vector<corpus::FileMeasurement> found =
            corpus::measure_directory(root, label.empty() ? root_label(root) : label, error);
        if (!error.empty()) {
            std::fprintf(stderr, "tessera_corpus_measure: %s\n", error.c_str());
            return error.find("is not a directory") != std::string::npos ? 2 : 1;
        }
        for (corpus::FileMeasurement& measurement : found) {
            measurements.push_back(std::move(measurement));
        }
    }

    const corpus::Report report = corpus::summarize(std::move(measurements));
    std::string text = corpus::format_report(report);
    if (!verbose) {
        // Drop the per-file list but keep the summary and the tables.
        const std::size_t files_at = text.find("\nfiles:\n");
        if (files_at != std::string::npos) {
            text.resize(files_at + 1);
        }
    }

    if (out_path.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "tessera_corpus_measure: cannot write %s\n", out_path.c_str());
        return 1;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good() ? 0 : 1;
}
