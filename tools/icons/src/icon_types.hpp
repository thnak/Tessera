// SPDX-License-Identifier: MIT
//
// Shared value types for the Iconify import: what an import produces, and the attribution entry the
// generated notices are built from.

#pragma once

#include "icon_license.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tessera::icons {

// One generated file, relative to the output root. `bytes` is the exact content, so a run can be
// compared byte-for-byte without touching the filesystem.
struct EmittedFile {
    std::string path;  // '/'-separated
    std::string bytes; // LF endings, no timestamps
};

struct ImportedIcon {
    std::string prefix;
    std::string name;
    std::string asset; // embedded asset name, e.g. "icons/tabler/home.svg"
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool monochrome = false;
};

struct ImportResult {
    std::vector<ImportedIcon> icons; // sorted by prefix then name
    std::vector<EmittedFile> files;  // sorted by path
};

// One set's contribution to the generated attribution.
struct AttributionSet {
    std::string prefix;
    std::string display_name;
    std::string author;
    std::string author_url;
    std::string license;
    std::string license_url;
    LicenseClass license_class = LicenseClass::Permissive;
    std::vector<std::string> icons; // imported icon names, sorted
};

} // namespace tessera::icons
