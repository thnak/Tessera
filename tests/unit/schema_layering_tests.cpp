// SPDX-License-Identifier: MIT

// Layering rule of docs/specs/jsx-intrinsics-v1/machine-readable.md section 4: `tessera_schema` is
// a leaf. Its sources include no project header (only its own and the standard library) and its
// CMake target links no project library.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_SCHEMA_SOURCE_DIR
#error "TESSERA_SCHEMA_SOURCE_DIR must be defined by the build"
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
    const fs::path root = TESSERA_SCHEMA_SOURCE_DIR;
    check(fs::exists(root), "the schema source directory exists");

    int files = 0;
    std::vector<std::string> sources;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto extension = entry.path().extension();
        if (!entry.is_regular_file() ||
            (extension != ".hpp" && extension != ".cpp" && extension != ".h")) {
            continue;
        }
        ++files;
        if (entry.is_regular_file() && extension == ".cpp") {
            sources.push_back(entry.path().filename().string());
        }
        for (const std::string& include : quoted_includes(entry.path())) {
            check(starts_with(include, "tessera/schema/") || include.find('/') == std::string::npos,
                  "the schema includes only its own headers: " + include + " in " +
                      entry.path().string());
        }
    }
    check(files > 0, "the schema scan finds source files");

    const std::string cmake = read_file(root / "CMakeLists.txt");
    check(cmake.find("add_library(tessera_schema STATIC") != std::string::npos,
          "the schema target is a static library");
    check(cmake.find("target_link_libraries(tessera_schema") == std::string::npos,
          "the schema target links no project library");
    for (const std::string& source : sources) {
        check(cmake.find(source) != std::string::npos,
              "the CMake target builds every schema source: " + source);
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "schema layering tests passed\n";
    return 0;
}
