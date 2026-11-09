// SPDX-License-Identifier: MIT

// Layering rule of docs/design/jsx-ir-lowering.md section 6: the IR and codegen never include
// parser (or any other project) headers. The CMake side of the rule, no linked project library,
// is a configure-time error in src/compiler/ir/CMakeLists.txt.

#include "ir_test_util.hpp"

#include <filesystem>
#include <fstream>
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

void check_directory(const fs::path& root, const std::string& name, bool expect_files) {
    int files = 0;
    if (fs::exists(root)) {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            const auto ext = entry.path().extension();
            if (!entry.is_regular_file() || (ext != ".hpp" && ext != ".cpp" && ext != ".h")) {
                continue;
            }
            ++files;
            for (const auto& include : project_includes(entry.path())) {
                const bool allowed = include.rfind("tessera/compiler/ir/", 0) == 0 ||
                                     include.find('/') == std::string::npos;
                check(allowed, name + ": " + entry.path().filename().string() +
                                   " includes a header outside the IR: " + include);
            }
        }
    }
    if (expect_files) {
        check(files > 0, name + ": the scan found source files (wrong source directory?)");
    }
}

} // namespace

int main() {
    const fs::path root = TESSERA_COMPILER_SOURCE_DIR;
    check_directory(root / "ir", "ir", true);
    check_directory(root / "codegen", "codegen", false);
    return finish("ir layering tests");
}
