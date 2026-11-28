// SPDX-License-Identifier: MIT

// Conformance corpus for the formatter (ADR-061 section 2 and "Consequences": a grammar form the
// printer does not print fails the build). Every .tsx file the repository ships under the listed
// directories is run through format_source: a file that parses must format and satisfy the three
// properties; a file that does not parse must be refused with TESSERA_FMT_SYNTAX.

#include "source_format_test_support.hpp"
#include "tessera/compiler/jsx/parser.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace source_format_test {

namespace fs = std::filesystem;

namespace {

std::vector<fs::path> corpus_files() {
    const fs::path root = TESSERA_SOURCE_DIR;
    std::vector<fs::path> files;
    for (const char* directory :
         {"samples", "packages", "tests/fixtures", "tools/reference", "designer/web/src"}) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root / directory, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->is_directory(ec) && it->path().filename() == "node_modules") {
                it.disable_recursion_pending();
            } else if (it->is_regular_file(ec) && it->path().extension() == ".tsx") {
                files.push_back(it->path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

void run_corpus_tests() {
    const std::vector<fs::path> files = corpus_files();
    check(files.size() > 100, "the corpus is found (" + std::to_string(files.size()) + " files)");
    std::size_t formatted = 0;
    std::size_t refused = 0;
    for (const fs::path& path : files) {
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const std::string text = buffer.str();
        const std::string name = fs::relative(path, TESSERA_SOURCE_DIR).generic_string();
        const auto parsed = tessera::compiler::jsx::parse_module(text);
        if (parsed.root != nullptr && !parsed.has_errors()) {
            ++formatted;
            check_properties(text, "corpus " + name);
        } else {
            ++refused;
            const auto result = format_tsx(text);
            check(!result.ok() && result.diagnostics[0].code == "TESSERA_FMT_SYNTAX",
                  "corpus " + name + ": a file that does not parse is refused as syntax");
        }
    }
    std::cout << "source_format corpus: " << formatted << " formatted, " << refused << " refused\n";
    // A corpus that quietly stopped reaching the printer would pass; pin the floor.
    check(formatted >= 80, "enough corpus files reach the printer (" + std::to_string(formatted) +
                               " formatted, " + std::to_string(refused) + " refused)");
}

} // namespace source_format_test
