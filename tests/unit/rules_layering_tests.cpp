// SPDX-License-Identifier: MIT

// Layering rule of docs/design/jsx-ir-lowering.md section 6: analysis sits between the front end
// and lowering, so tessera_react_rules never sees the IR. The CMake side is a configure-time error
// in src/compiler/rules/CMakeLists.txt; this checks the sources and that file's link line.

#include "ir_test_util.hpp" // only check()/finish(); it does not pull in the IR library

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef TESSERA_COMPILER_SOURCE_DIR
#error "TESSERA_COMPILER_SOURCE_DIR must be defined by the build"
#endif

namespace {

using namespace ir_test;
namespace fs = std::filesystem;

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

} // namespace

int main() {
    const fs::path root = fs::path(TESSERA_COMPILER_SOURCE_DIR) / "rules";
    int files = 0;
    if (fs::exists(root)) {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            const auto ext = entry.path().extension();
            if (!entry.is_regular_file() || (ext != ".hpp" && ext != ".cpp" && ext != ".h")) {
                continue;
            }
            ++files;
            for (const auto& include : project_includes(entry.path())) {
                const bool allowed = include.rfind("tessera/compiler/jsx/", 0) == 0 ||
                                     include.rfind("tessera/compiler/rules/", 0) == 0 ||
                                     include.find('/') == std::string::npos;
                check(allowed,
                      entry.path().filename().string() +
                          " includes a header outside the front end and the rules: " + include);
            }
        }
    }
    check(files > 0, "the scan found source files (wrong source directory?)");

    std::ifstream cmake(root / "CMakeLists.txt");
    std::stringstream text;
    text << cmake.rdbuf();
    const std::string content = text.str();
    check(!content.empty(), "the rules CMakeLists.txt was read");
    const auto link = content.find("target_link_libraries(tessera_react_rules");
    check(link != std::string::npos, "the link line was found");
    if (link != std::string::npos) {
        const auto end = content.find(')', link);
        check(content.substr(link, end - link).find("tessera_ir") == std::string::npos,
              "tessera_react_rules does not link tessera_ir");
    }
    return finish("rules layering tests");
}
