// SPDX-License-Identifier: MIT

// Walking a corpus directory. Kept apart from the measurement stages so the CLI and the golden test
// measure the same set of files with the same names: a second walk would eventually disagree about
// which files are in the corpus, and the golden would then pin a different set than the tool
// reports.

#include "measure.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tessera::compiler::corpus {
namespace {

namespace fs = std::filesystem;

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    out = text.str();
    return true;
}

// Every `.tsx` file under `root`, as a path relative to it with `/` separators, sorted. The sort is
// what makes the report deterministic across filesystems with different directory order.
std::vector<std::string> corpus_files(const fs::path& root) {
    std::vector<std::string> found;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end; !error && it != end;
         it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != ".tsx") {
            continue;
        }
        found.push_back(it->path().lexically_relative(root).generic_string());
    }
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace

std::vector<FileMeasurement> measure_directory(const std::string& root, const std::string& label,
                                               std::string& error) {
    std::vector<FileMeasurement> measurements;
    std::error_code code;
    if (!fs::is_directory(fs::path(root), code)) {
        error = root + " is not a directory";
        return measurements;
    }
    for (const std::string& relative : corpus_files(fs::path(root))) {
        std::string text;
        if (!read_file(fs::path(root) / relative, text)) {
            error = "cannot read " + root + "/" + relative;
            return {};
        }
        measurements.push_back(measure_source(label + "/" + relative, text));
        measure_pipeline(measurements.back(), text);
    }
    return measurements;
}

} // namespace tessera::compiler::corpus
