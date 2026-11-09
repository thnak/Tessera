// SPDX-License-Identifier: MIT

// Layering rule of docs/design/jsx-ir-lowering.md section 6: lowering is the only layer that
// includes the JSX AST, the analysis result and the IR; the IR includes neither the front end nor
// the analysis, and the lowering target links exactly those three libraries (section 6,
// "tessera_lowering links front end, rules and tessera_ir"). Slice 6 added the analysis include on
// purpose: lowering takes `(Module, AnalyzedModule)` (section 3).

#include "lowering_test_util.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_COMPILER_SOURCE_DIR
#error "TESSERA_COMPILER_SOURCE_DIR must be defined by the build"
#endif

namespace {

namespace fs = std::filesystem;
using namespace lowering_test;

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

// Project includes are written with quotes; angle brackets are the standard library.
std::vector<std::string> project_includes(const fs::path& file) {
    std::vector<std::string> found;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find_first_not_of(" \t");
        if (hash == std::string::npos || line.compare(hash, 8, "#include") != 0) {
            continue;
        }
        const auto open = line.find('"');
        const auto close = line.find('"', open + 1);
        if (open != std::string::npos && close != std::string::npos) {
            found.push_back(line.substr(open + 1, close - open - 1));
        }
    }
    return found;
}

bool has_source(const fs::path& directory) {
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        const auto extension = entry.path().extension();
        if (entry.is_regular_file() &&
            (extension == ".hpp" || extension == ".cpp" || extension == ".h")) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    const fs::path root = TESSERA_COMPILER_SOURCE_DIR;
    const fs::path lowering = root / "lowering";
    check(fs::exists(lowering), "the lowering directory exists");
    check(has_source(lowering), "the lowering scan finds source files");

    int files = 0;
    bool saw_jsx = false;
    bool saw_rules = false;
    bool saw_ir = false;
    for (const auto& entry : fs::recursive_directory_iterator(lowering)) {
        const auto extension = entry.path().extension();
        if (!entry.is_regular_file() ||
            (extension != ".hpp" && extension != ".cpp" && extension != ".h")) {
            continue;
        }
        ++files;
        for (const std::string& include : project_includes(entry.path())) {
            const bool own = starts_with(include, "tessera/compiler/lowering/");
            const bool jsx = starts_with(include, "tessera/compiler/jsx/");
            // Section 6 of the design notes: lowering links the analysis pass too.
            const bool rules = starts_with(include, "tessera/compiler/rules/");
            const bool ir = starts_with(include, "tessera/compiler/ir/");
            check(own || jsx || rules || ir || include.find('/') == std::string::npos,
                  "lowering includes only the AST, the analysis, the IR or its own headers: " +
                      include);
            saw_jsx = saw_jsx || jsx;
            saw_rules = saw_rules || rules;
            saw_ir = saw_ir || ir;
        }
    }
    check(files > 0, "the scan found lowering source files");
    check(saw_jsx, "lowering reads the JSX AST");
    check(saw_rules, "lowering reads the analysis result");
    check(saw_ir, "lowering builds the IR");

    const std::string cmake = read_file((lowering / "CMakeLists.txt").string());
    check(cmake.find("target_link_libraries(tessera_lowering PUBLIC tessera_jsx_frontend "
                     "tessera_ir tessera_react_rules)") != std::string::npos,
          "tessera_lowering links exactly the front end, the IR and the analysis");

    // The other direction: nothing under src/compiler/ir includes the front end or the analysis.
    for (const auto& entry : fs::recursive_directory_iterator(root / "ir")) {
        const auto extension = entry.path().extension();
        if (!entry.is_regular_file() ||
            (extension != ".hpp" && extension != ".cpp" && extension != ".h")) {
            continue;
        }
        for (const std::string& include : project_includes(entry.path())) {
            check(!starts_with(include, "tessera/compiler/jsx/"),
                  "the IR never includes the front end: " + include);
            check(!starts_with(include, "tessera/compiler/rules/"),
                  "the IR never includes the analysis: " + include);
        }
    }

    return finish("lowering layering tests");
}
