// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/property_dirty.hpp"

namespace tessera::ui::animation {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

} // namespace

bool is_layout_property(std::string_view property) {
    // Border color is paint-only: only the border widths and styles feed the layout boxes. Treating
    // it as layout would relayout the scene for a color fade (and make color interpolation request
    // layout work, which V1 does not do).
    if (property == "border-color" || property == "border-top-color" ||
        property == "border-right-color" || property == "border-bottom-color" ||
        property == "border-left-color") {
        return false;
    }
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
           starts_with(property, "justify-") || starts_with(property, "inset-inline-") ||
           property == "top" || property == "right" || property == "bottom" || property == "left";
}

bool is_essential_animation_property(std::string_view property) {
    return is_layout_property(property) || property == "transform" || property == "opacity";
}

bool is_non_essential_animation(const style::KeyframeBlock& block) {
    bool any = false;
    for (const style::KeyframeStep& step : block.steps) {
        for (const style::Declaration& declaration : step.declarations) {
            if (is_essential_animation_property(declaration.property)) {
                return false;
            }
            any = true;
        }
    }
    return any;
}

Dirty dirty_for_property(std::string_view property) {
    if (is_layout_property(property)) {
        return Dirty::Layout | Dirty::Paint | Dirty::Render;
    }
    return Dirty::Paint | Dirty::Render;
}

Dirty dirty_for_declarations(const std::vector<style::Declaration>& declarations) {
    Dirty dirty = Dirty::Clean;
    for (const style::Declaration& declaration : declarations) {
        dirty = dirty | dirty_for_property(declaration.property);
    }
    return dirty;
}

bool paint_dirties_subtree(std::string_view property) {
    // A node's opacity multiplies each primitive its descendants emit (paint-pass.md section
    // 14.1), so fading a parent changes the children's pixels even though `opacity` is not an
    // inherited CSS property. A layout property re-dirties its subtree through layout instead.
    return property == "opacity";
}

bool declarations_dirty_paint_subtree(const std::vector<style::Declaration>& declarations) {
    for (const style::Declaration& declaration : declarations) {
        if (paint_dirties_subtree(declaration.property)) {
            return true;
        }
    }
    return false;
}

} // namespace tessera::ui::animation
