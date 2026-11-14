// SPDX-License-Identifier: MIT
//
// Shared helpers for the two `tessera icons` test executables (the import/golden contract and the
// license/subset gate contract). Header-only so both compile the same assertions.

#pragma once

#include "icon_import.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::icons::test_support {

namespace fs = std::filesystem;

inline bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "icons: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] inline bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// A request over the pinned fixture sets (the only input; nothing is fetched from the network).
[[nodiscard]] inline ImportRequest fixture_request(std::vector<std::string> references) {
    ImportRequest request;
    request.iconify_root = TESSERA_ICONS_FIXTURE_DIR;
    request.references = std::move(references);
    return request;
}

[[nodiscard]] inline std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Sorted relative paths of every file under `root`; empty when the directory is missing.
[[nodiscard]] inline std::vector<std::string> list_files(const fs::path& root) {
    std::vector<std::string> paths;
    std::error_code error;
    if (!fs::exists(root, error)) {
        return paths;
    }
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, error)) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        paths.push_back(fs::relative(entry.path(), root).generic_string());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

[[nodiscard]] inline const EmittedFile* find_file(const ImportResult& result,
                                                  std::string_view path) {
    for (const EmittedFile& file : result.files) {
        if (file.path == path) {
            return &file;
        }
    }
    return nullptr;
}

} // namespace tessera::icons::test_support
