// SPDX-License-Identifier: MIT
//
// Pinned Iconify set reader (ADR-047 item 8). The input is the on-disk shape the pinned Iconify
// packages ship: <root>/<prefix>/info.json (set metadata and license) and
// <root>/<prefix>/icons.json (the icon bodies). Nothing here touches the network: the pinned
// directory is the input, so the import is offline and reproducible.

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::icons {

// One icon as Iconify stores it: an SVG fragment plus the viewBox it was drawn in.
struct IconSource {
    std::string name;
    std::string body;
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct IconSet {
    std::string prefix;
    std::string display_name;
    std::string author;
    std::string author_url;
    std::string license_title;
    std::string license_spdx;
    std::string license_url;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool palette = false;
    std::vector<IconSource> icons; // sorted by name

    [[nodiscard]] const IconSource* find(std::string_view name) const;
};

// Reads one pinned set. Fails (with a message naming the file) on missing or malformed metadata, a
// missing icon body, or an alias that carries a transform the converter cannot apply.
[[nodiscard]] std::expected<IconSet, std::string> load_icon_set(const std::string& root,
                                                                std::string_view prefix);

// The license exactly as the set records it: the SPDX identifier when present, else the title.
[[nodiscard]] std::string declared_license(const IconSet& set);

} // namespace tessera::icons
