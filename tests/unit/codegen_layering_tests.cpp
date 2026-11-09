// SPDX-License-Identifier: MIT

// Layering rule of AGENTS.md non-negotiable 4 and docs/design/codegen.md: codegen reads the IR
// only. Its sources include their own headers, `tessera/compiler/ir/` headers or the standard
// library, and the CMake target links exactly tessera_ir. The IR never includes codegen.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_COMPILER_SOURCE_DIR
#error "TESSERA_COMPILER_SOURCE_DIR must be defined by the build"
#endif

namespace {

namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::vector<std::string> quoted_includes(const fs::path& file) {
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

std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

} // namespace

int main() {
    const fs::path root = TESSERA_COMPILER_SOURCE_DIR;
    const fs::path codegen = root / "codegen";
    check(fs::exists(codegen), "the codegen source directory exists");

    int files = 0;
    bool saw_ir = false;
    for (const auto& entry : fs::recursive_directory_iterator(codegen)) {
        const auto extension = entry.path().extension();
        if (!entry.is_regular_file() ||
            (extension != ".hpp" && extension != ".cpp" && extension != ".h")) {
            continue;
        }
        ++files;
        for (const std::string& include : quoted_includes(entry.path())) {
            const bool own = starts_with(include, "tessera/compiler/codegen/");
            const bool ir = starts_with(include, "tessera/compiler/ir/");
            check(own || ir || include.find('/') == std::string::npos,
                  "codegen includes only the IR or its own headers: " + include + " in " +
                      entry.path().string());
            saw_ir = saw_ir || ir;
        }
    }
    check(files > 0, "the codegen scan finds source files");
    check(saw_ir, "codegen reads the IR");

    const std::string cmake = read_file(codegen / "CMakeLists.txt");
    check(cmake.find("target_link_libraries(tessera_codegen PUBLIC tessera_ir)") !=
              std::string::npos,
          "tessera_codegen links exactly tessera_ir");

    for (const auto& entry : fs::recursive_directory_iterator(root / "ir")) {
        const auto extension = entry.path().extension();
        if (!entry.is_regular_file() ||
            (extension != ".hpp" && extension != ".cpp" && extension != ".h")) {
            continue;
        }
        for (const std::string& include : quoted_includes(entry.path())) {
            check(!starts_with(include, "tessera/compiler/codegen/"),
                  "the IR never includes codegen: " + include);
        }
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen layering tests passed\n";
    return 0;
}
