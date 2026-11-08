// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/property_dirty.hpp"

namespace tessera::ui::animation {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

} // namespace

bool is_layout_property(std::string_view property) {
    if (property == "display" || property == "position" || property == "box-sizing" ||
        property == "width" || property == "height" || property == "min-width" ||
        property == "min-height" || property == "max-width" || property == "max-height" ||
        property == "aspect-ratio" || property == "gap" || property == "row-gap" ||
        property == "column-gap" || property == "font-family" || property == "font-size" ||
        property == "font-weight" || property == "font-style" || property == "line-height" ||
        property == "letter-spacing" || property == "word-spacing" || property == "white-space" ||
        property == "text-align" || property == "text-transform" || property == "writing-mode") {
        return true;
    }
    return starts_with(property, "margin-") || starts_with(property, "padding-") ||
           starts_with(property, "border-") || starts_with(property, "flex-") ||
           starts_with(property, "grid-") || starts_with(property, "align-") ||
           starts_with(property, "justify-") || property == "top" || property == "right" ||
           property == "bottom" || property == "left";
}

Dirty dirty_for_property(std::string_view property) {
    if (is_layout_property(property)) {
        return Dirty::Layout | Dirty::Paint | Dirty::Render;
    }
    return Dirty::Paint | Dirty::Render;
}

Dirty dirty_for_declarations(const std::vector<style::Declaration>& declarations) {
    Dirty dirty = Dirty::None;
    for (const style::Declaration& declaration : declarations) {
        dirty = dirty | dirty_for_property(declaration.property);
    }
    return dirty;
}

} // namespace tessera::ui::animation
