// SPDX-License-Identifier: MIT

#include "resources.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <fstream>
#include <iostream>
#include <iterator>

namespace gallery {

namespace style = tessera::ui::style;

namespace {

std::optional<std::string> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

std::optional<Resources> load_resources() {
    const auto button_css = read_file(TESSERA_GALLERY_BUTTON_CSS);
    const auto icon_css = read_file(TESSERA_GALLERY_ICON_CSS);
    const auto switch_css = read_file(TESSERA_GALLERY_SWITCH_CSS);
    const auto checkbox_css = read_file(TESSERA_GALLERY_CHECKBOX_CSS);
    const auto radio_css = read_file(TESSERA_GALLERY_RADIO_CSS);
    const auto safe_area_css = read_file(TESSERA_GALLERY_SAFE_AREA_CSS);
    const auto gallery_css = read_file(TESSERA_GALLERY_CSS);
    const auto form_css = read_file(TESSERA_GALLERY_FORM_CSS);
    const auto dash_css = read_file(TESSERA_GALLERY_DASH_CSS);
    const auto swatch_css = read_file(TESSERA_GALLERY_SWATCH_CSS);
    const auto lists_css = read_file(TESSERA_GALLERY_LISTS_CSS);
    const auto tables_css = read_file(TESSERA_GALLERY_TABLES_CSS);
    const auto theme_json = read_file(TESSERA_GALLERY_THEME);
    if (!button_css || !icon_css || !switch_css || !checkbox_css || !radio_css || !safe_area_css ||
        !gallery_css || !form_css || !dash_css || !swatch_css || !lists_css || !tables_css ||
        !theme_json) {
        std::cerr << "gallery: cannot read the library CSS, gallery CSS or theme asset\n";
        return std::nullopt;
    }
    const auto theme = style::parse_theme_asset(*theme_json);
    if (!theme) {
        std::cerr << "gallery: theme asset: " << theme.error().message << "\n";
        return std::nullopt;
    }
    Resources resources;
    // The library sheet is the Button, Icon, Switch, Checkbox and Radio looks (one `tessera-ui`
    // layer).
    resources.library_sheet =
        style::prepare_stylesheet(*button_css + "\n" + *icon_css + "\n" + *switch_css + "\n" +
                                  *checkbox_css + "\n" + *radio_css);
    resources.safe_area_sheet = style::prepare_stylesheet(*safe_area_css);
    resources.gallery_sheet = style::prepare_stylesheet(*gallery_css);
    auto form_module = gallery::load_form_module(*form_css);
    if (!form_module) {
        return std::nullopt;
    }
    resources.form_module = *form_module; // the scan's scoped text, as FormPage's `styles.x`
    resources.form_sheet = style::prepare_stylesheet(resources.form_module.css);
    resources.dash_sheet = style::prepare_stylesheet(*dash_css);
    resources.swatch_sheet = style::prepare_stylesheet(*swatch_css);
    resources.lists_sheet = style::prepare_stylesheet(*lists_css);
    resources.tables_sheet = style::prepare_stylesheet(*tables_css);
    resources.theme_blob = style::serialize_theme_asset(*theme);
    return resources;
}

style::CompiledStyleSheet make_sheet(const Resources& resources) {
    const std::vector<style::StylesheetSource> sources{
        {"button.css", &resources.library_sheet, style::Origin::Author, true},
        {"safe-area.css", &resources.safe_area_sheet, style::Origin::Author, true},
        {"gallery.css", &resources.gallery_sheet, style::Origin::Author, false},
        {"form.module.css", &resources.form_sheet, style::Origin::Author, false},
        {"dash.css", &resources.dash_sheet, style::Origin::Author, false},
        {"swatch.css", &resources.swatch_sheet, style::Origin::Author, false},
        {"lists.css", &resources.lists_sheet, style::Origin::Author, false},
        {"tables.css", &resources.tables_sheet, style::Origin::Author, false},
    };
    return style::CompiledStyleSheet(sources);
}

std::string root_classes(const Args& args) {
    std::string classes = "g-root";
    if (args.density == "compact") {
        classes += " g-compact";
    }
    if (args.rtl) {
        classes += " g-rtl";
    }
    if (args.text_scale == 125) {
        classes += " g-scale-125";
    } else if (args.text_scale == 150) {
        classes += " g-scale-150";
    }
    return classes;
}

} // namespace gallery
