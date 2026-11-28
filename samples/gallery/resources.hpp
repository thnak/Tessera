// SPDX-License-Identifier: MIT
//
// The gallery's resources, shared by the native window/capture driver (main.cpp) and the web driver
// (web_main.cpp, issue #1023): the library, page and theme files read once from the paths the build
// defines, the stylesheet they compose, and the root class string the host switches set.

#pragma once

#include "args.hpp"
#include "form_module.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gallery {

// Everything the page needs besides the generated component: the library Button sheet (layered in
// `tessera-ui`), the gallery sheet, and the theme asset blob. Built once from the repository files.
struct Resources {
    tessera::ui::style::PreparedStyleSheet library_sheet;
    tessera::ui::style::PreparedStyleSheet safe_area_sheet;
    tessera::ui::style::PreparedStyleSheet gallery_sheet;
    tessera::ui::style::PreparedStyleSheet form_sheet;
    FormModule form_module;
    tessera::ui::style::PreparedStyleSheet dash_sheet;
    tessera::ui::style::PreparedStyleSheet swatch_sheet;
    tessera::ui::style::PreparedStyleSheet lists_sheet;
    tessera::ui::style::PreparedStyleSheet tables_sheet;
    tessera::ui::style::PreparedStyleSheet settings_sheet;
    std::vector<std::uint8_t> theme_blob;
};

// Reads every file; prints the reason and returns nullopt when one cannot be read or parsed.
[[nodiscard]] std::optional<Resources> load_resources();

[[nodiscard]] tessera::ui::style::CompiledStyleSheet make_sheet(const Resources& resources);

[[nodiscard]] std::string root_classes(const Args& args);

} // namespace gallery
