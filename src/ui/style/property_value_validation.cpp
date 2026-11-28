// SPDX-License-Identifier: MIT

#include "substitution.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/background_layers.hpp"
#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/cursor_value.hpp"
#include "tessera/ui/style/font_variant.hpp"
#include "tessera/ui/style/font_variant_alternates.hpp"
#include "tessera/ui/style/grid_values.hpp"
#include "tessera/ui/style/list_style.hpp"
#include "tessera/ui/style/mask_layers.hpp"
#include "tessera/ui/style/property_value_validation.hpp"
#include "tessera/ui/style/text_decoration.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

using tessera::detail::lower;
using tessera::detail::trim;
namespace {

// True when `word` appears in `text` as a whole keyword (not inside `inset-x` or a longer name).
bool has_keyword(std::string_view text, std::string_view word) {
    std::size_t at = text.find(word);
    while (at != std::string_view::npos) {
        const bool left_ok = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) ||
                                          text[at - 1] == '-');
        const std::size_t end = at + word.size();
        const bool right_ok =
            end >= text.size() ||
            !(std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '-');
        if (left_ok && right_ok) {
            return true;
        }
        at = text.find(word, at + 1);
    }
    return false;
}

// The scroll runtime properties are single-keyword grammars in V1 (docs/specs/stylesheet-v1.md
// section 11); the two-value forms CSS allows are not implemented and are reported as unsupported.
bool is_single_keyword(std::string_view value, std::initializer_list<std::string_view> allowed) {
    const std::string_view token = trim(value);
    if (token.empty() || token.find_first_of(" \t\r\n\f") != std::string_view::npos) {
        return false;
    }
    return std::find(allowed.begin(), allowed.end(), token) != allowed.end();
}

bool is_scroll_style_property(std::string_view property) {
    return property == "scroll-behavior" || property == "overscroll-behavior" ||
           property == "overscroll-effect" || property == "scrollbar-gutter";
}

bool scroll_style_value_supported(std::string_view property, std::string_view value) {
    if (property == "scroll-behavior") {
        return is_single_keyword(value, {"auto", "smooth"});
    }
    if (property == "overscroll-behavior") {
        return is_single_keyword(value, {"auto", "contain", "none"});
    }
    // The exact names of docs/specs/animation-v1.md section 7 (OverscrollEffect).
    if (property == "overscroll-effect") {
        return is_single_keyword(value, {"none", "glow", "stretch", "bounce", "platform"});
    }
    if (property == "scrollbar-gutter") {
        return is_single_keyword(value, {"auto", "stable"});
    }
    return true;
}

const char* scroll_style_message(std::string_view property) {
    if (property == "scroll-behavior") {
        return "scroll-behavior takes one of auto or smooth";
    }
    if (property == "overscroll-behavior") {
        return "overscroll-behavior takes one of auto, contain or none";
    }
    if (property == "overscroll-effect") {
        return "overscroll-effect takes one of none, glow, stretch, bounce or platform";
    }
    return "scrollbar-gutter takes one of auto or stable";
}

bool is_grid_style_property(std::string_view property) {
    return property == "grid-template-columns" || property == "grid-template-rows" ||
           property == "grid-column" || property == "grid-row" || property == "grid-auto-flow" ||
           property == "justify-self" || property == "justify-items";
}

bool grid_style_value_supported(std::string_view property, std::string_view value) {
    if (property == "grid-template-columns" || property == "grid-template-rows") {
        return parse_grid_template(value).valid;
    }
    if (property == "grid-column" || property == "grid-row") {
        return parse_grid_line(value).valid;
    }
    if (property == "grid-auto-flow") {
        return is_supported_grid_auto_flow(value);
    }
    return is_supported_grid_alignment(value);
}

const char* grid_style_message(std::string_view property) {
    if (property == "grid-template-columns" || property == "grid-template-rows") {
        return "expected none or a list of track sizes (lengths, percentages, fr, auto, "
               "min-content, max-content, minmax(), repeat(<integer>, ...)); subgrid, named lines, "
               "auto-fill/auto-fit and named areas are not supported";
    }
    if (property == "grid-column" || property == "grid-row") {
        return "expected auto, <line>, span <n> or <line> / <line>; named lines are not supported";
    }
    if (property == "grid-auto-flow") {
        return "grid-auto-flow takes one of row or column; dense packing is not supported";
    }
    return "expected one of start, end, center, stretch, normal, left, right, flex-start, "
           "flex-end, auto or legacy";
}

// The text-decoration longhands and `vertical-align` (docs/design/text-decoration.md): empty when
// `value` is drawable, otherwise the message naming the accepted grammar.
const char* text_decoration_value_error(std::string_view property, std::string_view value) {
    if (property == "text-decoration-line") {
        return parse_text_decoration_line(value)
                   ? nullptr
                   : "text-decoration-line takes none or a set of underline, overline and "
                     "line-through without repeats; blink and the error lines are not supported";
    }
    if (property == "text-decoration-style") {
        return is_supported_text_decoration_style(value)
                   ? nullptr
                   : "only solid text-decoration-style is supported";
    }
    if (property == "text-decoration-color") {
        const std::string text = lower(trim(value));
        return text == "currentcolor" || parse_color(text).ok
                   ? nullptr
                   : "text-decoration-color takes currentColor or a colour";
    }
    if (property == "text-decoration-thickness") {
        return is_supported_text_decoration_thickness(value)
                   ? nullptr
                   : "text-decoration-thickness takes auto, from-font or a px/em/rem length";
    }
    if (property == "vertical-align") {
        return is_supported_vertical_align(value)
                   ? nullptr
                   : "vertical-align takes baseline, sub, super or a px/em/rem length; top, "
                     "middle, bottom, text-top, text-bottom and percentages are not supported";
    }
    return nullptr;
}

} // namespace

bool validate_property_value(const Declaration& declaration, std::string_view property,
                             std::vector<StyleDiagnostic>& out) {
    const std::string value = lower(declaration.value);
    if (property == "list-style-type" || property == "list-style-position") {
        // A `var()` reference is substituted after this check; judge the resolved text then.
        if (detail::has_substitution_function(value)) {
            return true;
        }
        const bool ok = property == "list-style-type" ? parse_list_style_type(value).has_value()
                                                      : is_supported_list_style_position(value);
        if (!ok) {
            out.push_back(StyleDiagnostic{
                StyleDiagnosticKind::UnsupportedValue, declaration.offset, std::string(property),
                property == "list-style-type"
                    ? "list-style-type takes none, disc, circle, square, decimal, lower-alpha, "
                      "upper-alpha, lower-roman or upper-roman; other counter styles are not drawn"
                    : "list-style-position takes outside or inside"});
        }
        return ok;
    }
    if (property == kFontVariantAlternatesProperty) {
        if (detail::has_substitution_function(value) || parse_font_variant_alternates(value)) {
            return true;
        }
        out.push_back(StyleDiagnostic{
            StyleDiagnosticKind::UnsupportedValue, declaration.offset, std::string(property),
            "font-variant-alternates takes normal or stylistic(), swash(), ornaments(), "
            "annotation(), styleset(), character-variant() and historical-forms, each at most "
            "once (docs/design/text.md section 3.1); the declaration is ignored"});
        return false;
    }
    if (property.starts_with("font-variant-")) {
        // The five keyword groups (font_variant.hpp). An unknown or conflicting value drops the
        // declaration whole, as CSS does for an invalid value. A `var()` is judged once resolved.
        constexpr FontVariantGroup kGroups[] = {
            FontVariantGroup::Ligatures, FontVariantGroup::Numeric, FontVariantGroup::Caps,
            FontVariantGroup::EastAsian, FontVariantGroup::Position};
        for (const FontVariantGroup group : kGroups) {
            if (font_variant_property(group) != property) {
                continue;
            }
            if (detail::has_substitution_function(value) ||
                parse_font_variant_value(group, value)) {
                return true;
            }
            out.push_back(StyleDiagnostic{
                StyleDiagnosticKind::UnsupportedValue, declaration.offset, std::string(property),
                std::string(property) +
                    " takes normal or its keywords without conflicts (docs/design/text.md "
                    "section 3.1); the declaration is ignored"});
            return false;
        }
        return true;
    }
    if (property.starts_with("text-decoration-") || property == "vertical-align") {
        // A `var()` reference is substituted after this check; judge the resolved text then.
        if (detail::has_substitution_function(value)) {
            return true;
        }
        if (const char* message = text_decoration_value_error(property, value)) {
            out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset,
                                          std::string(property), message});
            return false;
        }
        return true;
    }
    // `cursor` (issue #191) is a closed keyword set plus the `url()` list V1 does not implement.
    // The declaration is kept on an unsupported form: the computed value falls back to the initial
    // `auto`, so the scene still has a cursor to read while the diagnostic names the form. Its
    // grammar lives in cursor_value.cpp, shared with `@supports` and the scene sync.
    if (property == "cursor") {
        report_unsupported_cursor_value(value, declaration.offset, out);
        return true;
    }
    // The layer-list grammar is the one in background_layers.hpp (issue #614): a comma-separated
    // list of `linear-gradient()` and `none`, painted bottom to top. A value outside it keeps the
    // declaration (the paint pass then draws no image) and says why, once. A `var()` reference is
    // substituted after this check, so it is judged on the resolved text instead.
    if (property == "background-image" || property == "mask-image") {
        const std::string_view text = trim(value);
        if (!detail::has_substitution_function(text) && !parse_background_layers(text)) {
            out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset,
                                          std::string(property),
                                          "only a comma-separated list of linear-gradient() or "
                                          "radial-gradient() (optionally repeating-, 2 to 8 colour "
                                          "stops) and none is supported; no image is painted"});
        }
        return true;
    }
    // The mask geometry lists (mask_layers.hpp). An invalid list drops the declaration, so the
    // computed value keeps the initial one and the diagnostic names the form.
    if (property == "mask-size" || property == "mask-position" || property == "mask-repeat" ||
        property == "mask-mode" || property == "mask-origin" || property == "mask-clip" ||
        property == "mask-composite") {
        const std::string_view text = trim(value);
        if (detail::has_substitution_function(text)) {
            return true;
        }
        const bool ok = property == "mask-size"       ? parse_mask_size_list(text).has_value()
                        : property == "mask-position" ? parse_mask_position_list(text).has_value()
                        : property == "mask-repeat"   ? parse_mask_repeat_list(text).has_value()
                        : property == "mask-mode"     ? parse_mask_mode_list(text).has_value()
                        : property == "mask-origin"   ? parse_mask_origin_list(text).has_value()
                        : property == "mask-clip"     ? parse_mask_clip_list(text).has_value()
                                                      : parse_mask_composite_list(text).has_value();
        if (!ok) {
            out.push_back(StyleDiagnostic{
                StyleDiagnosticKind::UnsupportedValue, declaration.offset, std::string(property),
                "mask-size takes auto, cover, contain or 1 to 2 px/% values; mask-position takes "
                "1 to 4 keyword/px/% values; mask-repeat takes repeat-x, repeat-y or 1 to 2 of "
                "repeat, space, round, no-repeat; mask-mode takes alpha, luminance or "
                "match-source; mask-origin takes border-box, padding-box or content-box and "
                "mask-clip those "
                "or no-clip; mask-composite takes add, subtract, intersect or exclude (per layer, "
                "comma-separated, at most 8)"});
        }
        return ok;
    }
    if (property == "text-shadow" && has_keyword(value, "inset")) {
        // `text-shadow` has no inset form at all (CSS Text Decoration 3 section 3), so this is a
        // grammar error rather than an unimplemented V1 form; the declaration is dropped.
        out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset,
                                      std::string(property),
                                      "text-shadow has no inset form; the declaration is ignored"});
        return false;
    }
    if (property == "transform" &&
        (value.find("3d(") != std::string::npos || has_keyword(value, "perspective") ||
         value.find("perspective(") != std::string::npos)) {
        out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset,
                                      std::string(property),
                                      "3D transform functions are not supported in V1"});
    }
    if (!is_scroll_style_property(property)) {
        if (!is_grid_style_property(property)) {
            return true;
        }
        // A custom-property reference is substituted after this check; keep the declaration so the
        // computed-value pass can judge the resolved text.
        if (detail::has_substitution_function(value)) {
            return true;
        }
        if (grid_style_value_supported(property, value)) {
            return true;
        }
        out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset,
                                      std::string(property), grid_style_message(property)});
        return false;
    }
    // A custom-property reference is substituted after this check; keep the declaration so the
    // computed-value pass can judge the resolved text.
    if (detail::has_substitution_function(value)) {
        return true;
    }
    if (scroll_style_value_supported(property, value)) {
        return true;
    }
    out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset,
                                  std::string(property), scroll_style_message(property)});
    return false;
}

} // namespace tessera::ui::style
