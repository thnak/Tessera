// SPDX-License-Identifier: MIT

// Golden test of the schema dump (docs/specs/jsx-intrinsics-v1/machine-readable.md section 4):
// tests/golden/schema/intrinsics.json and tessera.d.ts are compared byte for byte. Changing them is
// a deliberate schema change: run `schema_golden_tests --write` and review the diff against the
// spec tables.

#include "core.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifndef TESSERA_SCHEMA_GOLDEN_DIR
#error "TESSERA_SCHEMA_GOLDEN_DIR must be defined by the build"
#endif

namespace {

namespace fs = std::filesystem;

int failures = 0;

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

void report_difference(const std::string& name, std::string_view expected,
                       std::string_view actual) {
    std::size_t line = 1;
    std::size_t begin = 0;
    while (begin < expected.size() && begin < actual.size() && expected[begin] == actual[begin]) {
        if (expected[begin] == '\n') {
            ++line;
        }
        ++begin;
    }
    std::cerr << "FAIL: " << name << " is out of date (first difference at byte " << begin
              << ", line " << line << ")\n"
              << "  run: schema_golden_tests --write  (a deliberate schema change)\n";
    ++failures;
}

void compare(const fs::path& directory, const std::string& name, std::string_view expected,
             bool update) {
    const fs::path path = directory / name;
    if (update) {
        if (!write_file(path, expected)) {
            std::cerr << "FAIL: cannot write " << path.string() << "\n";
            ++failures;
        }
        return;
    }
    const std::string actual = read_file(path);
    if (actual != expected) {
        report_difference(name, expected, actual);
    }
}

void check_output_shape(std::string_view json, std::string_view dts) {
    if (json.size() < 4 || json.substr(0, 2) != "{\n" || json.substr(json.size() - 2) != "}\n") {
        std::cerr << "FAIL: intrinsics.json is not a JSON object with one trailing newline\n";
        ++failures;
    }
    if (json.find('\r') != std::string_view::npos) {
        std::cerr << "FAIL: intrinsics.json contains a CR\n";
        ++failures;
    }
    if (dts.find("namespace JSX") == std::string_view::npos ||
        dts.find("interface IntrinsicElements") == std::string_view::npos ||
        dts.find("export interface ViewProps") == std::string_view::npos ||
        dts.find("export type HTMLDivElement = ViewElement;") == std::string_view::npos) {
        std::cerr << "FAIL: tessera.d.ts is missing a generated declaration family\n";
        ++failures;
    }
    std::ptrdiff_t depth = 0;
    for (const char c : dts) {
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
        }
    }
    if (depth != 0) {
        std::cerr << "FAIL: tessera.d.ts has unbalanced braces\n";
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--write";
    const fs::path directory = TESSERA_SCHEMA_GOLDEN_DIR;
    const std::string json = tessera::schema::dump::json();
    const std::string dts = tessera::schema::dump::dts();
    check_output_shape(json, dts);
    compare(directory, "intrinsics.json", json, update);
    compare(directory, "tessera.d.ts", dts, update);
    if (update) {
        if (failures != 0) {
            return 1;
        }
        std::cout << "schema goldens written to " << directory.string() << "\n";
        return 0;
    }
    if (failures != 0) {
        std::cerr << failures << " golden check(s) failed\n";
        return 1;
    }
    std::cout << "schema golden tests passed\n";
    return 0;
}
